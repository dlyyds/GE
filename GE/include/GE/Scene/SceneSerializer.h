#pragma once

#include <string>
#include <memory>

namespace GE {

class Scene;
class VulkanDevice;
class VulkanResourceCache;
class Texture;
class Mesh;
class Material;

/**
 * @brief 场景序列化器 —— 基于 YAML 格式保存和加载场景。
 *
 * 将场景中的所有实体及其组件序列化为 YAML 文件，或从 YAML 文件反序列化重建场景。
 * 支持的组件：TagComponent、TransformComponent、SpriteRendererComponent、
 *            MeshRendererComponent、CameraComponent、PointLightComponent、
 *            DirectionalLightComponent、AmbientLightComponent、RigidBodyComponent、
 *            BoxColliderComponent、SphereColliderComponent、CapsuleColliderComponent。
 *
 * 材质：网格自带的子网格默认材质（随模型加载）不落场景；实体对子网格的材质
 * 覆写（MeshRendererComponent.materialOverrides）会落盘，两种形态二选一——
 * 有 `.gemat` 源文件的写引用（`Material: materials/x.gemat`，材质资产独立落盘、
 * 可跨场景复用），无源文件的写内联内容。形状由 MaterialSerializer 统一提供。
 * SpriteRenderer 的纹理同样保存其采样器参数。
 *
 * ScriptComponent 不参与序列化（运行时行为，无法持久化）。
 *
 * 资源处理：
 * - 纹理、材质、网格都以文件路径字符串形式保存在 YAML 中
 * - 全部走全局管理器（Renderer::GetTextureManager / GetMaterialManager /
 *   Renderer::GetMeshManager），按内容/路径去重、生命周期由引擎管理，
 *   不随场景销毁，避免场景组件持有悬空资源指针
 *
 * 使用方式：
 * @code
 *   SceneSerializer serializer(scene);
 *   serializer.Serialize("scenes/test.scene");   // 保存（相对资源根，自动解析）
 *   serializer.Deserialize("scenes/test.scene"); // 加载（会清空当前场景）
 * @endcode
 */
class SceneSerializer {
public:
    /**
     * @brief 构造场景序列化器。
     * @param scene 目标场景（序列化时作为源，反序列化时作为目标——会被清空）
     */
    explicit SceneSerializer(Scene *scene);

    ~SceneSerializer();

    SceneSerializer(const SceneSerializer &) = delete;
    SceneSerializer &operator=(const SceneSerializer &) = delete;

    /**
     * @brief 将场景序列化为 YAML 文件。
     * @param filepath 输出文件路径（建议使用 .scene 或 .yaml 扩展名）
     * @return true  保存成功
     * @return false 保存失败（文件无法写入等）
     */
    bool Serialize(const std::string &filepath);

    /**
     * @brief 从 YAML 文件反序列化场景。
     *
     * 会先清空当前场景（m_Scene）中的所有实体，然后根据 YAML 内容重建。
     * 纹理、材质、网格全部通过全局管理器加载，Serializer 不持有资源所有权。
     *
     * @param filepath 场景文件路径
     * @return true  加载成功
     * @return false 加载失败（文件不存在、格式错误等）
     */
    bool Deserialize(const std::string &filepath);

private:
    Scene *m_Scene = nullptr;
};

} // namespace GE
