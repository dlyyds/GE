#pragma once

#include <string>
#include <unordered_map>
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
 *            MeshComponent、MaterialComponent、CameraComponent、PointLightComponent、
 *            DirectionalLightComponent、AmbientLightComponent、RigidBodyComponent、
 *            BoxColliderComponent、SphereColliderComponent。
 *
 * 材质序列化：完整保存纹理槽位（Albedo / Normal / Emissive）与全部浮点标量参数
 * （如 shininess、specularStrength），按内容去重（内容相同的材质复用同一实例）。
 *
 * ScriptComponent 不参与序列化（运行时行为，无法持久化）。
 *
 * 资源处理：
 * - 纹理和网格以文件路径字符串形式保存在 YAML 中
 * - 纹理 / 材质使用全局管理器（Renderer::GetTextureManager / GetMaterialManager）
 * - 网格由 SceneSerializer 内部缓存（网格加载也需要 device，且网格资源生命周期
 *   通常随场景，与纹理/材质不同）
 *
 * 使用方式：
 * @code
 *   SceneSerializer serializer(scene, &device);
 *   serializer.Serialize("assets/scenes/test.scene");  // 保存
 *   serializer.Deserialize("assets/scenes/test.scene"); // 加载（会清空当前场景）
 * @endcode
 */
class SceneSerializer {
public:
    /**
     * @brief 构造场景序列化器。
     * @param scene  目标场景（序列化时作为源，反序列化时作为目标——会被清空）
     * @param device Vulkan 设备指针（用于加载网格，仅反序列化需要；
     *               如果只做序列化可传 nullptr）
     */
    explicit SceneSerializer(Scene *scene,
                             VulkanDevice *device = nullptr);

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
     * 网格资源存储在 SceneSerializer 内部；纹理和材质使用全局管理器。
     *
     * @param filepath 场景文件路径
     * @return true  加载成功
     * @return false 加载失败（文件不存在、格式错误等）
     */
    bool Deserialize(const std::string &filepath);

    /**
     * @brief 清空内部加载的网格缓存。
     *
     * 注意：调用前请确保没有组件还在引用这些网格指针。
     * 纹理和材质由全局管理器管理，不受此方法影响。
     */
    void ClearLoadedResources();

private:
    Scene *m_Scene = nullptr;
    VulkanDevice *m_Device = nullptr;

    // 反序列化时加载的网格（同路径去重，由 SceneSerializer 持有所有权）
    // 纹理和材质走全局管理器（Renderer::GetTextureManager / GetMaterialManager）
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_LoadedMeshes;

    /**
     * @brief 获取或加载指定路径的网格（内部去重）。
     * @return 网格指针，失败返回 nullptr
     */
    Mesh *GetOrLoadMesh(const std::string &filepath);
};

} // namespace GE
