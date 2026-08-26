#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <functional>


#define GLM_ENABLE_EXPERIMENTAL
#include <string>
#include <vector>
#include <unordered_map>
#include <random>
#include <cstdint>
#include <memory>
#include <glm/gtx/quaternion.hpp>

#include "entt.hpp"

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
 * @brief 关节标记组件 —— 挂在 glTF 关节 node 对应的实体上，标出它是骨骼关节。
 *
 * 关节实体本身是普通实体（带 TransformComponent，位于骨架层级中），由
 * SkinComponent.joints 引用；jointIndex 是该关节在 skin.joints 里的序号（0..N-1）。
 * 关节的 world 变换每帧由 Scene::UpdateWorldTransforms DFS 计算，蒙皮据此变形。
 */
struct JointComponent {
    int jointIndex = 0; ///< 该关节在 skin.joints 里的索引（0..N-1）

    JointComponent() = default;
    explicit JointComponent(int idx) : jointIndex(idx) {}
};

/**
 * @brief 一条 glTF skin 的定义（关节链 + 逆绑定矩阵）。
 *
 * 同一条 skin 可被多个 node 引用（如整车多块蒙皮共享一骨架，valhalla 107 个
 * node 共用 skin[0]）。各 SkinComponent 以 shared_ptr 共享同一份 SkinDef，
 * 数据只存一份；Scene::UpdateSkins 每帧按 SkinDef 去重，仅计算并上传一次
 * 关节矩阵，多 node 复用同一结果，避免逐组件重复计算/上传。
 *
 * joints 存原始 entt::entity 句柄（与 TransformComponent::parent 一致），避免
 * Components.h 里 Entity 类型不完整而无法整存 std::vector<Entity> 的问题。
 * inverseBindMatrices 是常量（绑定姿态快照），与 joints 一一对应。
 */
struct SkinDef {
    std::vector<entt::entity> joints;           ///< 关节实体句柄（有序，索引 = skin.joints 序）
    std::vector<glm::mat4>    inverseBindMatrices; ///< 逆绑定矩阵（与 joints 一一对应）
};

/**
 * @brief 皮肤组件 —— 挂在「带 mesh 且被骨骼驱动的 node」实体上。
 *
 * 记录驱动该网格的共享皮肤定义（SkinDef）+ 关联网格。加载期由 GLTFSceneImporter
 * 填充，运行时 Scene::UpdateSkins 每帧据此计算 jointMatrix = world × IBM 并上传 GPU。
 *
 * skin 为空（nullptr）表示未接入皮肤定义（如手动 Add Component、或导入时皮肤
 * 被跳过），绘制时按静态网格处理。joints()/inverseBindMatrices() 便捷访问器在
 * skin 为空时返回空表，保证读取安全。
 */
struct SkinComponent {
    std::shared_ptr<SkinDef> skin;              ///< 共享皮肤定义（同 glTF skin 各 node 共享；null = 未接入）
    Mesh    *MeshPtr = nullptr;                 ///< 被本皮肤驱动的网格（可选，可从绘制时取）
    bool     RequiresJointUpload = true;        ///< 脏标记：需重新上传关节矩阵（首版每帧重算，暂未用）

    SkinComponent() = default;
    SkinComponent(const SkinComponent &) = default;

    /// @brief 共享关节表（skin 为空时返回空表，读取安全）
    [[nodiscard]] const std::vector<entt::entity> &joints() const {
        static const std::vector<entt::entity> empty;
        return skin ? skin->joints : empty;
    }

    /// @brief 共享逆绑定矩阵（skin 为空时返回空表，读取安全）
    [[nodiscard]] const std::vector<glm::mat4> &inverseBindMatrices() const {
        static const std::vector<glm::mat4> empty;
        return skin ? skin->inverseBindMatrices : empty;
    }
};


// ============================================================
// 动画组件（数据层，详见 docs/骨骼动画实现计划书.md §4 阶段 A）
// ============================================================

/// 一条动画通道：动某个节点的某条路径（TRS）
struct AnimationChannel {
    enum class Interp : uint8_t { Linear = 0, Step = 1, CubicSpline = 2 };
    enum class Path : uint8_t { Translation = 0, Rotation = 1, Scale = 2 };

    int         nodeIndex = -1;    ///< glTF 目标节点索引（导入期解析成实体）
    Path        path = Path::Translation;
    Interp      interp = Interp::Linear;
    // 键帧（按路径类型分存，避免每帧类型转换；rotation 已转 glm::quat 内存序）
    std::vector<float>     times;           ///< 键帧时间（秒，单调递增）
    std::vector<glm::vec3> vecKeys;         ///< translation / scale 值（每键帧一个）
    std::vector<glm::quat> quatKeys;        ///< rotation 值（每键帧一个，wxyz 序）
};

/// 动画片段（模型级共享资源：与 SkinDef 同构，跨实体按 "path#N" 去重）
struct AnimationClip {
    std::string                     name;
    std::string                     source;   ///< 源键 "path#N"（加载期由 AnimationClipManager 填，序列化回读用）
    float                           duration = 0.0f;
    std::vector<AnimationChannel>   channels;
};

/// 绑定到场景的动画实例：clip（共享键帧）+ 本次解析的目标实体
struct ClipInstance {
    std::shared_ptr<AnimationClip> clip;
    std::vector<entt::entity>      channelTargets;  ///< 与 clip->channels 一一对应（未解析为 null）
};

/// 动画组件：挂在带骨架的角色实体上（与 SkinComponent 同实体）
struct AnimationComponent {
    std::vector<ClipInstance> clips;   ///< 模型全部动画（mint 1 条）
    size_t  active = 0;                 ///< 当前播放 clip 索引
    float   time = 0.0f;                ///< 播放时间（秒）
    float   speed = 1.0f;               ///< 播放倍速
    bool    playing = true;             ///< 是否在播
    bool    loop = true;                ///< 是否循环

    AnimationComponent() = default;
    AnimationComponent(const AnimationComponent &) = default;

    /// 便捷访问当前 clip（无动画返回 nullptr）
    [[nodiscard]] const AnimationClip *activeClip() const {
        return (active < clips.size()) ? clips[active].clip.get() : nullptr;
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
    std::string Name;               ///< 环境名，对应 environments/<Name>/ 子文件夹
    bool Enabled = true;            ///< 环境总开关（关则天空盒 + IBL 一并关闭）
    bool SkyboxEnabled = true;      ///< 天空盒背景开关
    bool IBLEnabled = true;         ///< IBL 环境光开关

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