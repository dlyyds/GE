#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <utility>
#include <functional>


#define GLM_ENABLE_EXPERIMENTAL
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <cstdint>
#include <memory>
#include <glm/gtx/quaternion.hpp>

#include "entt.hpp"

#include "Core/KeyCodes.h"

#include "Render/Camera.h"
#include "Physics/PhysicsTypes.h"
#include "Scene/AnimationComponents.h"

namespace GE {

class Texture; // 前向声明，避免引入整个 Texture 头文件
class Mesh; // 前向声明，避免引入整个 Mesh 头文件
class Material; // 前向声明，避免引入整个 Material 头文件
class Entity; // 前向声明，供 ScriptComponent 回调签名使用
class Timestep; // 前向声明，供 ScriptComponent 回调签名使用


struct TagComponent {
    std::string Tag;

    TagComponent() = default;

    TagComponent(const TagComponent &) = default;

    explicit TagComponent(std::string tag) : Tag(std::move(tag)) {
    }
};

struct TransformComponent {
    // 局部 TRS（作者数据）：编辑器/动画/物理只写这组字段
    glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
    // 内部统一存四元数，避免欧拉角万向锁（绕中间轴 ±90° 时自由度丢失）
    // 需要欧拉角时仅在编辑器显示边界用 Get/SetRotationEuler 转换
    glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

    // 世界矩阵缓存（派生值）：只能由 Scene::UpdateWorldTransforms 每帧 DFS 写入，
    // 其他路径禁止写（四条纪律 1）；仅在本帧 DFS 之后有效（纪律 4），序列化显式排除（纪律 3）。
    // 平凡可拷贝，不破坏 EnTT 对组件的 POD 要求。
    glm::mat4 worldMatrix = glm::mat4(1.0f);

    // 父实体句柄（entt::null = 根）。唯一真相在组件指针，Scene 侧的反向索引是派生缓存。
    // EnTT 句柄带版本位，实体销毁后再复用不会产生悬垂引用。
    entt::entity parent = entt::null;

    TransformComponent() = default;

    TransformComponent(const TransformComponent &) = default;

    explicit TransformComponent(const glm::vec3 &translation)
        : Translation(translation),
          worldMatrix(glm::translate(glm::mat4(1.0f), translation)) {
    }

    /// @brief 取欧拉角（弧度，GLM 的 XYZ 顺序），仅供编辑器显示使用
    [[nodiscard]] glm::vec3 GetRotationEuler() const {
        return glm::eulerAngles(Rotation);
    }

    /// @brief 用欧拉角（弧度）设置旋转，仅供编辑器写入使用
    void SetRotationEuler(const glm::vec3 &euler) {
        Rotation = glm::quat(euler);
    }

    /// @brief 局部矩阵 = Translation × Rotation × Scale（层级化后不再等同世界矩阵）
    [[nodiscard]] glm::mat4 GetLocalMatrix() const {
        const glm::mat4 rotation = glm::toMat4(Rotation);
        return glm::translate(glm::mat4(1.0f), Translation)
               * rotation
               * glm::scale(glm::mat4(1.0f), Scale);
    }

    /// @brief 世界矩阵（读缓存的 worldMatrix，由 Scene 每帧 DFS 更新；父子层级下子实体正确跟随父级）
    [[nodiscard]] const glm::mat4 &GetWorldMatrix() const {
        return worldMatrix;
    }
};

/// 生成 128 位随机标识字符串（32 个十六进制字符）。
/// 供 IDComponent 使用：实体创建时分配，用于序列化时跨文件稳定引用父实体（UUID 方案）。
/// 使用函数内静态 mt19937_64，以 random_device 一次性播种。
inline std::string GenerateUUID() {
    static std::mt19937_64 generator{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> distribution;
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (int i = 0; i < 4; ++i) {
        const uint64_t word = distribution(generator);
        for (int j = 0; j < 16; ++j) {
            result.push_back(kHex[(word >> (j * 4)) & 0xF]);
        }
    }
    return result;
}

/**
 * @brief 实体唯一标识组件 —— 持久化引用标识。
 *
 * 创建实体时由 Scene::CreateEntity 分配随机 UUID；随实体序列化落盘。
 * 场景级联引用（父实体）在序列化文件里以父实体的 UUID 字符串承接，
 * 反序列化走「先全建 → 记 ID → 再二次遍历接 parent」的两遍流程，
 * 重排 / 增删实体不影响引用语义。
 *
 * 不参与渲染 / 物理遍历；运行时也可按 ID 反查实体（Scene::FindEntityByID）。
 */
struct IDComponent {
    std::string UUID;

    IDComponent() = default;

    IDComponent(const IDComponent &) = default;

    explicit IDComponent(std::string uuid)
        : UUID(std::move(uuid)) {
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
    Texture *SpriteTexture = nullptr; ///< 精灵纹理（可选，为 null 时绘制纯色矩形）
    bool IsUI = false; ///< 是否为 UI 精灵（true=屏幕空间无深度，false=世界空间有深度）

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
 * @brief 包围盒组件 —— 手动放置的模型局部空间 AABB（实体级粗剔除用）。
 *
 * 视锥剔除对蒙皮实体整段跳过（绑定姿势 AABB 追不上动画变形），导致屏幕外
 * 的动画角色照样渲染；静态网格的 mesh AABB 已够用，通常只需给含蒙皮的模型
 * 根实体摆放此盒。剔除时盒经实体世界矩阵变换到世界空间，完全在视锥外则连
 * 该实体的整棵子实体一并跳过（见 Scene::RenderMeshes3D 的子树级预筛）。
 *
 * Center/Size 为局部空间（相对实体原点，与 Mesh::GetAABB 的模型空间语义
 * 一致）。未摆放（Size 全 0）时 IsValid()==false，不参与剔除。编辑器端用
 * gizmo 拖拽角/边调整，序列化进 .scene。
 */
struct BoundingBoxComponent {
    glm::vec3 Center = {0.0f, 0.0f, 0.0f}; ///< 局部空间中心
    glm::vec3 Size = {0.0f, 0.0f, 0.0f}; ///< 局部空间边长（任一轴 <= 0 视为未摆放）

    BoundingBoxComponent() = default;

    BoundingBoxComponent(const BoundingBoxComponent &) = default;

    BoundingBoxComponent(const glm::vec3 &center, const glm::vec3 &size)
        : Center(center), Size(size) {
    }

    /// 有效判定：三轴均 > 0 才算摆放好（默认 0 尺寸为未放置）
    [[nodiscard]] bool IsValid() const {
        return Size.x > 0.0f && Size.y > 0.0f && Size.z > 0.0f;
    }

    /// 局部空间最小角（供剔除变换与 gizmo 的 localBounds 用）
    [[nodiscard]] glm::vec3 minCorner() const {
        return Center - Size * 0.5f;
    }

    /// 局部空间最大角
    [[nodiscard]] glm::vec3 maxCorner() const {
        return Center + Size * 0.5f;
    }
};


/// public 字段值类型（脚本 PUBLIC_FIELDS 声明；序列化为字符串 Type 标签，见 SceneSerializer）。
enum class ScriptFieldType : uint8_t {
    None = 0,
    Number,
    Bool,
    String
};

/// 单个 public 字段的值载荷（Type 决定哪个成员有效；按类型冗余存值便于面板/脚本直读）。
struct ScriptPublicField {
    ScriptFieldType Type = ScriptFieldType::None;
    float Number = 0.0f;
    bool Bool = false;
    std::string String;
};

/**
 * @brief 脚本组件 —— 挂载 Lua 脚本文件到实体（纯数据，可序列化）。
 *
 * 运行态全部在 Scene::ScriptEngine（Lua 5.4 + sol2，共享一个 Lua 状态）。
 * ScriptPath 为 assets/scripts/ 下的相对路径（含 .lua 后缀）；空路径 = 未挂载。
 * PublicFields 是脚本 `PUBLIC_FIELDS` 声明字段的本实体取值：编辑器面板编辑、
 * 随场景落盘，脚本经注入的 `public.get(name)` 实时读取（引擎不改运行中实例）。
 * 行为约定见 docs/Lua脚本系统计划书.md。
 */
struct ScriptComponent {
    std::string ScriptPath; ///< assets/scripts/ 下相对路径（含 .lua 后缀）
    bool Enabled = true;    ///< 是否启用（false 时跳过 OnUpdate）
    /// public 字段值：<名, 值>；键与脚本 PUBLIC_FIELDS 声明对齐（缺省由 ScriptEngine 补默认）。
    std::unordered_map<std::string, ScriptPublicField> PublicFields;

    ScriptComponent() = default;

    ScriptComponent(const ScriptComponent &) = default;

    explicit ScriptComponent(std::string path)
        : ScriptPath(std::move(path)) {
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
    Mesh *MeshPtr = nullptr; ///< 网格资源指针（可选，为 null 时不绘制）

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
 * 包含一个完整的 Camera 实例，支持 Orbit（轨道）和 FreeLook（自由视角）两种模式。
 * Primary 标志用于标记场景中的主相机，渲染器会使用主相机的视图投影矩阵进行渲染。
 * FixedAspectRatio 控制是否随窗口大小自动调整宽高比。
 *
 * 与 TransformComponent 的关系：
 * - 若实体挂载了 TransformComponent，可在逻辑层将 Transform 的 Translation/Rotation 同步到 Camera
 * - 也可直接使用 Camera 内置的交互（OnEvent、MoveForward 等）独立控制
 */
struct CameraComponent {
    Camera CameraInstance; ///< 相机实例（包含投影、视图、交互等完整功能）
    bool Primary = true; ///< 是否为主相机（场景中第一个主相机会被渲染器使用）
    bool FixedAspectRatio = false; ///< 是否固定宽高比（false 时随窗口大小自动调整）

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
    float RadiusInv = 0.5f; ///< 光源影响半径倒数（衰减系数，越大衰减越快）

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
 * 光线方向取 Transform 前向向量（即 -Z 轴经过旋转后的方向），指向光源（朝太阳）；
 * 其相反方向（-lightDir）即光传播方向（从光源指向被照物），由 Scene 取反后存入
 * LightParams.dirLightDirection，着色器再取反还原为朝光方向做 N·L。
 * Color 的 rgb 分量表示光源颜色，a 分量表示光源强度。
 *
 * 由 Scene 在渲染前收集方向光组件，取场景中第一个方向光作为主方向光，
 * 传递给 Renderer3D 的 LightParams。场景中存在多个方向光时，仅第一个生效。
 */
struct DirectionalLightComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 光源颜色(rgb) + 强度(a)
    bool CastShadow = true;                  ///< 是否投方向光阴影（默认开）

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


/**
 * @brief 环境组件 —— 统一管理场景的环境（天空盒背景 + IBL 环境光）。
 *
 * 天空盒与 IBL 环境光来自同一 HDRI 源，应天然一致，故用一个组件承载。
 * Name 为环境名，对应 assets/environments/<Name>/ 子文件夹；Scene 按命名
 * 约定推导三张图（prefilter 用于 IBL、skybox 用于背景、brdf_lut 共享）。
 *
 * 三个开关分工：
 * - Enabled：环境总开关，关闭则天空盒 + IBL 一并关闭。
 * - SkyboxEnabled：天空盒背景开关。
 * - IBLEnabled：IBL 环境光开关。
 *
 * 环境是场景级属性（不依赖实体的 Transform），取场景中第一个 EnvironmentComponent
 * 作为环境。由 Scene 在渲染前读取，调用 Renderer3D::SetEnvironmentMap /
 * SetSkyboxEnabled / SetIBLEnabled 驱动。可随场景序列化。
 */
struct EnvironmentComponent {
    std::string Name; ///< 环境名，对应 environments/<Name>/ 子文件夹
    bool Enabled = true; ///< 环境总开关（关则天空盒 + IBL 一并关闭）
    bool SkyboxEnabled = true; ///< 天空盒背景开关
    bool IBLEnabled = true; ///< IBL 环境光开关
    float IBLIntensity = 1.0f; ///< IBL 环境光强度（整体缩放 diffuse + specular 贡献）

    EnvironmentComponent() = default;

    EnvironmentComponent(const EnvironmentComponent &) = default;

    /**
     * @brief 指定环境名的构造函数。
     */
    explicit EnvironmentComponent(std::string name)
        : Name(std::move(name)) {
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
    float Mass = 1.0f; ///< 质量（kg，静态体/运动学体忽略）
    float Friction = 0.6f; ///< 摩擦系数（0~1）
    float Restitution = 0.0f; ///< 弹性系数（0~1）
    float LinearDamping = 0.05f; ///< 线性阻尼
    float AngularDamping = 0.05f; ///< 角阻尼
    bool IsSensor = false; ///< 是否为触发器（不产生物理响应，只触发事件）

    // 运行时数据（不参与序列化）
    Physics::BodyID RuntimeBodyID{}; ///< Jolt Body 句柄（由 PhysicsWorld 设置）
    bool IsInitialized = false; ///< 是否已加入物理世界

    RigidBodyComponent() = default;

    RigidBodyComponent(const RigidBodyComponent &) = default;

    explicit RigidBodyComponent(Physics::RigidBodyType type) : Type(type) {
    }
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
    glm::vec3 Offset = {0.0f, 0.0f, 0.0f}; ///< 相对于刚体中心的偏移
    bool DrawDebug = true; ///< 是否在视口叠加绘制该碰撞体调试线框（可单独关闭）

    BoxColliderComponent() = default;

    BoxColliderComponent(const BoxColliderComponent &) = default;

    explicit BoxColliderComponent(const glm::vec3 &halfExtents) : HalfExtents(halfExtents) {
    }
};

/**
 * @brief 球体碰撞体组件。
 *
 * Radius 为球体半径，Offset 为碰撞体相对于刚体中心的偏移。
 * 实体上可挂载多个碰撞体组件，PhysicsWorld 会合并为复合形状。
 */
struct SphereColliderComponent {
    float Radius = 0.5f; ///< 半径
    glm::vec3 Offset = {0.0f, 0.0f, 0.0f}; ///< 偏移
    bool DrawDebug = true; ///< 是否在视口叠加绘制该碰撞体调试线框（可单独关闭）

    SphereColliderComponent() = default;

    SphereColliderComponent(const SphereColliderComponent &) = default;

    explicit SphereColliderComponent(float radius) : Radius(radius) {
    }
};

/**
 * @brief 胶囊碰撞体轴向 —— 胶囊主轴沿实体局部坐标的哪根轴放置。
 *
 * Jolt 的 CapsuleShape 原生沿局部 Y（= 引擎 up 轴）；当实体的 Transform
 * 带着旋转（例如 Z-up 模型经 90°X 旋转转正后局部 Z 才是模型的"上"）时，
 * 默认 Y 会让胶囊横躺，此时改选 X/Z 即可让胶囊贴住模型的实际朝向。
 * 缩放烘焙跟随所选轴：半高按该轴的分量缩放。
 */
enum class CapsuleAxis : uint8_t {
    Y = 0, ///< 沿局部 Y 轴（默认）
    X = 1, ///< 沿局部 X 轴
    Z = 2  ///< 沿局部 Z 轴
};

/**
 * @brief 胶囊碰撞体组件。
 *
 * 胶囊由中间圆柱段 + 上下两个半球帽组成（与 Jolt CapsuleShape 语义一致）。
 * Radius 为圆柱段半径，HalfHeight 为圆柱段半高（球帽球心在 ±HalfHeight 处，
 * 极点分别在 ±(HalfHeight + Radius)）。总长 = 2 × (HalfHeight + Radius)。
 * Axis 决定胶囊主轴沿实体局部坐标的哪根轴（默认 Y，见 CapsuleAxis）。
 * 缩放烘焙跟随所选轴：半径取三轴缩放最大值，半高乘所选轴的分量缩放
 * （与 BuildShapeForEntity / 调试线框一致）。
 * Offset 是碰撞体相对于刚体中心的偏移。
 *
 * 实体上可挂载多个碰撞体组件，PhysicsWorld 会合并为复合形状。
 */
struct CapsuleColliderComponent {
    float Radius = 0.5f; ///< 半径
    float HalfHeight = 0.5f; ///< 圆柱段半高（不含球帽）
    CapsuleAxis Axis = CapsuleAxis::Y; ///< 胶囊主轴方向（局部坐标）
    glm::vec3 Offset = {0.0f, 0.0f, 0.0f}; ///< 偏移
    bool DrawDebug = true; ///< 是否在视口叠加绘制该碰撞体调试线框（可单独关闭）

    CapsuleColliderComponent() = default;

    CapsuleColliderComponent(const CapsuleColliderComponent &) = default;

    explicit CapsuleColliderComponent(float radius) : Radius(radius) {
    }
};

/**
 * @brief 角色控制器组件 —— 基于 Jolt CharacterVirtual 的可操控角色。
 *
 * 与 RigidBodyComponent 二选一：角色实体只挂 TransformComponent +
 * CharacterControllerComponent，不挂 RigidBodyComponent（避免与 Kinematic 的
 * MoveKinematic 抢占位移）。CharacterVirtual 自管位置、重力和碰撞滑动，脚本
 * 只提供水平期望速度与跳跃请求，引擎每物理子步积分重力并驱动 ExtendedUpdate
 * （自动上楼 + 贴地吸附）。TransformComponent.Translation 即角色「脚底」位置
 * （胶囊底部对齐原点），与 CharacterVirtual 的 mPosition 语义一致。
 *
 * Axis 语义同 CapsuleColliderComponent：模型实际"上"不在局部 Y 时（如 Z-up 模型
 * 经旋转转正后），选 X/Z 让胶囊沿对应轴放置，且选轴应保证该轴经实体旋转后指向
 * 世界 up（角色仍沿世界 Y 行走/贴地，只旋转胶囊几何）。
 *
 * 运行时字段不参与 .scene 序列化（见 docs/角色控制器CharacterVirtual计划书.md）。
 */
struct CharacterControllerComponent {
    // 配置（参与序列化）
    float Radius        = 0.35f; ///< 胶囊半径（米）
    float Height        = 1.80f; ///< 胶囊总高（含两端半球，米）
    CapsuleAxis Axis    = CapsuleAxis::Y; ///< 胶囊主轴方向（局部坐标）
    glm::vec3 Offset    = {0.0f, 0.0f, 0.0f}; ///< 胶囊相对脚底的局部偏移（角色局部帧）
    float MaxSlopeAngle = 45.0f; ///< 可上坡最大倾角（度）
    float MaxJumpSpeed  = 5.0f;  ///< 跳跃初速（m/s，character.jump 使用）
    float GravityScale  = 1.0f;  ///< 重力缩放（仅影响角色自己，下落手感可单独调，不影响全局刚体）
    bool  FaceMovement  = true;  ///< 是否朝向水平移动方向（脚本有水平输入时绕 up 缓转）
    float TurnSpeed     = 540.0f; ///< 转向速率（度/秒，FaceMovement 生效时的最大偏航角速度）
    CapsuleAxis FrontAxis = CapsuleAxis::Z; ///< 模型前向基准轴（决定面朝方向对齐哪个局部轴）
    bool InvertFront = false;               ///< 前向轴取反：模型"脸"在 FrontAxis 的反方向时置 true

    // 运行时（不参与序列化）
    bool      IsInitialized = false;              ///< CharacterVirtual 已创建
    glm::vec3 WishVelocity  = {0.0f, 0.0f, 0.0f}; ///< 脚本每帧写入的水平期望速度（引擎只看 x/z）
    bool      JumpRequested = false;              ///< 脚本置位，引擎贴地时消费一次
    bool      IsGrounded    = false;              ///< 引擎每子步回写（贴地/斜坡/悬空）
    glm::vec3 Velocity      = {0.0f, 0.0f, 0.0f}; ///< 最近一次真实速度（character.get_velocity 回读）
    float     GroundNormalY = 1.0f;               ///< 最近一次地面法线 Y（character.get_ground_normal_y 回读）
    glm::quat BaseRotation  = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); ///< 面朝基准姿态（创建时捕获，不随之累加）
    float     FacingYaw     = 0.0f;               ///< 累计偏航角（弧度，绕世界 up；面朝逻辑每子步驱动）
    bool      FacingInit    = false;              ///< BaseRotation/FacingYaw 已初始化（重建不重捕获）

    CharacterControllerComponent() = default;

    CharacterControllerComponent(const CharacterControllerComponent &) = default;

    explicit CharacterControllerComponent(float radius) : Radius(radius) {
    }
};


/** 跟随相机视图模式（运行时 + 可序列化起始模式）。 */
enum class FollowCameraViewMode : uint8_t {
    FirstPerson = 0,   ///< 第一人称（默认）
    ThirdPerson = 1,   ///< 第三人称
};

/**
 * @brief 跟随相机组件 —— 挂在角色实体上，让主相机跟随角色视点。
 *
 * 与 CameraComponent 的关系：本组件【不】自带 Camera 实例，而是标记
 * "场景里那台 Primary Camera 跟着我"。渲染仍用 CameraComponent 的实例，
 * 本组件只在每帧末尾把相机位置钉到角色视点、把相机朝向（鼠标控制）
 * 回写为角色朝向。实现见 Scene::UpdateFollowCamera。
 *
 * 配置（参与 .scene 序列化）：
 *   Enabled          总开关（关闭则相机不跟随，角色也不被相机驱动朝向）
 *   StartMode        视图起始模式（First/Third）
 *   ToggleEnabled/ToggleKey  运行时切换开关与按键（默认 V）
 *   EyeOffset        第一人称视点偏移（角色局部系，绕 up 随朝向旋转）
 *   YawSpeed/PitchSpeed      鼠标偏航/俯仰灵敏度（度/像素）
 *   MinPitch/MaxPitch        俯仰上下限（度）
 *   InvertY                 俯仰反转（可选，默认 false）
 *   TargetOffset     第三人称看向的角色局部锚点
 *   Distance/MinDistance/MaxDistance  第三人称目标/钳位距离
 *   ShoulderOffset   过肩水平偏移
 *   CollisionEnabled/CollisionRadius/CollisionMargin  防穿墙配置（M2 启用）
 *   Smoothing        位置平滑阻尼系数
 *   ZoomSpeed        滚轮一格改变的距离
 *
 * 运行时（不参与序列化）：
 *   CurrentMode      当前 Play 模式
 *   CurrentDistance  当前运行时距离（滚轮修改）
 *   CurrentPos       当前相机位置（平滑跟随用）
 *   姿态主相机仍存在 CameraComponent 的 Camera 里。
 */

struct FollowCameraComponent {
    // —— 总开关 ——
    bool  Enabled    = true;      ///< 总开关

    // —— 视图模式（可序列化）——
    FollowCameraViewMode StartMode = FollowCameraViewMode::FirstPerson; ///< Play 启动时使用的模式
    bool  ToggleEnabled = true;      ///< 是否允许运行时切换
    KeyCode ToggleKey = Key::V;      ///< 切换键（默认 V）

    // —— 共用：鼠标视角（沿用现有字段）——
    glm::vec3 EyeOffset = {0.0f, 1.65f, 0.0f}; ///< 第一人称视点偏移（角色局部系，绕 up 随朝向旋转；纯第一人称 = +Y 抬高）
    float YawSpeed   = 0.10f;     ///< 偏航灵敏度（度/像素）
    float PitchSpeed = 0.10f;     ///< 俯仰灵敏度（度/像素）
    float MinPitch   = -89.0f;    ///< 俯仰下限（度）
    float MaxPitch   = 89.0f;     ///< 俯仰上限（度）
    bool  InvertY    = false;     ///< 俯仰是否反转

    // —— 第三人称 ——
    glm::vec3 TargetOffset   = {0.0f, 1.60f, 0.0f}; ///< 相机看向的角色局部锚点（脚底起抬高）
    float Distance           = 3.50f; ///< 目标距离（滚轮会运行时修改 CurrentDistance，不改此基值）
    float MinDistance        = 1.00f;
    float MaxDistance        = 12.0f;
    float ShoulderOffset     = 0.00f; ///< 过肩水平偏移（>0 右肩、<0 左肩；0 = 正中跟拍）
    bool  CollisionEnabled   = true;  ///< 是否启用相机防穿墙
    float CollisionRadius    = 0.25f; ///< 相机球体半径（卡碰撞用）
    float CollisionMargin    = 0.05f; ///< 离障碍物额外余量
    float Smoothing          = 8.0f;  ///< 位置/距离阻尼系数（越大越跟手）
    float ZoomSpeed          = 0.40f; ///< 滚轮一格改变的距离

    // —— 运行时（不参与 .scene 序列化）——
    FollowCameraViewMode CurrentMode = FollowCameraViewMode::FirstPerson; ///< Play 当前模式
    float CurrentDistance = -1.0f;   ///< 当前平滑距离；首次进入第三人称初始化为 Distance
    glm::vec3 CurrentPos  = {0,0,0}; ///< 当前相机位置（用于平滑跟随）
    bool ThirdPersonSnapPending = false; ///< 下一帧第三人称直接钉到目标位置（硬切，无过渡）

    FollowCameraComponent() = default;

    FollowCameraComponent(const FollowCameraComponent &) = default;

    explicit FollowCameraComponent(const glm::vec3 &eyeOffset) : EyeOffset(eyeOffset) {
    }
};

}