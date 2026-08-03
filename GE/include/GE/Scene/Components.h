#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <functional>


#define GLM_ENABLE_EXPERIMENTAL
#include <string>
#include <glm/gtx/quaternion.hpp>

#include "Render/Camera.h"

namespace GE {

class Texture;  // 前向声明，避免引入整个 Texture 头文件
class Mesh;     // 前向声明，避免引入整个 Mesh 头文件
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
 * @brief 脚本组件 —— 挂载到实体上的每帧回调。
 *
 * 轻量级脚本系统：通过 std::function 绑定一个每帧执行的回调，
 * 在 Scene::OnUpdate 中被调用，用于实现实体的行为逻辑。
 *
 * 回调签名：void(Timestep ts, Entity entity)
 * - ts: 时间步长
 * - entity: 该组件所属的实体，可在回调中读写其组件
 */
struct ScriptComponent {
    using Callback = std::function<void(Timestep, Entity)>;

    Callback OnUpdate;  ///< 每帧更新回调
    bool     Enabled = true; ///< 脚本是否启用（false 时跳过 OnUpdate 调用）

    ScriptComponent() = default;

    ScriptComponent(const ScriptComponent &) = default;

    explicit ScriptComponent(Callback callback)
        : OnUpdate(std::move(callback)) {
    }
};


/**
 * @brief 静态网格渲染组件 —— 描述一个 3D 网格的渲染属性。
 *
 * 与 TransformComponent 配合使用：Transform 决定位置/旋转/缩放，
 * MeshComponent 决定绘制什么网格、什么颜色。
 *
 * Mesh 和 Texture 使用裸指针引用，不拥有资源。资源由外部资源管理器（如 VulkanResourceCache）管理。
 * Color 为 RGBA 分量，白色 (1,1,1,1) 表示原样显示纹理/材质。
 * Texture 为 nullptr 时使用纯白色替代（相当于无纹理的纯色网格）。
 *
 * 由 Renderer3D 在 Scene::OnUpdate3D() 中遍历并绘制，
 * 支持深度测试、背面剔除和 Blinn-Phong 光照。
 */
struct MeshComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 叠加颜色（默认白色，即不染色）
    Mesh     *MeshPtr = nullptr;              ///< 网格资源指针（可选，为 null 时不绘制）
    Texture  *BaseTexture = nullptr;          ///< 主纹理指针（可选，为 null 时使用纯白色）

    MeshComponent() = default;

    MeshComponent(const MeshComponent &) = default;

    /**
     * @brief 仅指定颜色的构造函数（纯色网格，无网格资源）。
     */
    explicit MeshComponent(const glm::vec4 &color)
        : Color(color) {
    }

    /**
     * @brief 指定网格的构造函数（颜色默认白色，无纹理）。
     */
    explicit MeshComponent(Mesh *mesh)
        : MeshPtr(mesh) {
    }

    /**
     * @brief 同时指定网格和纹理的构造函数（颜色默认白色）。
     */
    MeshComponent(Mesh *mesh, Texture *texture)
        : MeshPtr(mesh), BaseTexture(texture) {
    }

    /**
     * @brief 同时指定网格和颜色的构造函数（无纹理）。
     */
    MeshComponent(Mesh *mesh, const glm::vec4 &color)
        : Color(color), MeshPtr(mesh) {
    }

    /**
     * @brief 同时指定网格、纹理和颜色的构造函数。
     */
    MeshComponent(Mesh *mesh, Texture *texture, const glm::vec4 &color)
        : Color(color), MeshPtr(mesh), BaseTexture(texture) {
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
 * 点光源数量受 Renderer3D::MAX_POINT_LIGHTS 限制，超出部分会被忽略。
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


}