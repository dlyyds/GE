#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <functional>


#define GLM_ENABLE_EXPERIMENTAL
#include <string>
#include <glm/gtx/quaternion.hpp>

namespace GE {

class Texture;  // 前向声明，避免引入整个 Texture 头文件
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
 * SpriteRendererComponent 决定显示什么纹理、什么颜色。
 *
 * 纹理使用裸指针引用，不拥有资源。纹理资源由外部资源管理器（如 VulkanResourceCache）管理。
 * Color 为 RGBA 分量，白色 (1,1,1,1) 表示原样显示纹理。
 */
struct SpriteRendererComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 叠加颜色（默认白色，即不染色）
    Texture  *SpriteTexture = nullptr;        ///< 精灵纹理（可选，为 null 时绘制纯色矩形）

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

    Callback OnUpdate; ///< 每帧更新回调

    ScriptComponent() = default;

    ScriptComponent(const ScriptComponent &) = default;

    explicit ScriptComponent(Callback callback)
        : OnUpdate(std::move(callback)) {
    }
};


}