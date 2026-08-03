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

/**
 * @brief 场景序列化器 —— 基于 YAML 格式保存和加载场景。
 *
 * 将场景中的所有实体及其组件序列化为 YAML 文件，或从 YAML 文件反序列化重建场景。
 * 支持的组件：TagComponent、TransformComponent、SpriteRendererComponent、
 *            MeshComponent、CameraComponent、PointLightComponent。
 *
 * ScriptComponent 不参与序列化（运行时行为，无法持久化）。
 *
 * 资源处理：
 * - 纹理和网格以文件路径字符串形式保存在 YAML 中
 * - 反序列化时自动从文件加载资源，同一资源路径只加载一次（内部去重）
 * - 加载的资源由 SceneSerializer 持有，调用方需保证 SceneSerializer 生命周期长于 Scene
 *
 * 使用方式：
 * @code
 *   SceneSerializer serializer(scene, &device, &cache);
 *   serializer.Serialize("assets/scenes/test.scene");  // 保存
 *   serializer.Deserialize("assets/scenes/test.scene"); // 加载（会清空当前场景）
 * @endcode
 */
class SceneSerializer {
public:
    /**
     * @brief 构造场景序列化器。
     * @param scene  目标场景（序列化时作为源，反序列化时作为目标——会被清空）
     * @param device Vulkan 设备指针（用于加载纹理/网格，仅反序列化需要；
     *               如果只做序列化可传 nullptr）
     * @param cache  Vulkan 资源缓存指针（用于 Sampler 去重，仅反序列化需要）
     */
    SceneSerializer(Scene *scene,
                    VulkanDevice *device = nullptr,
                    VulkanResourceCache *cache = nullptr);

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
     * 加载的纹理和网格资源存储在 SceneSerializer 内部。
     *
     * @param filepath 场景文件路径
     * @return true  加载成功
     * @return false 加载失败（文件不存在、格式错误等）
     */
    bool Deserialize(const std::string &filepath);

    /**
     * @brief 清空内部加载的资源缓存（纹理和网格）。
     *
     * 注意：调用前请确保没有组件还在引用这些资源的指针。
     */
    void ClearLoadedResources();

private:
    Scene *m_Scene = nullptr;
    VulkanDevice *m_Device = nullptr;
    VulkanResourceCache *m_ResourceCache = nullptr;

    // 反序列化时加载的资源（同路径去重，由 SceneSerializer 持有所有权）
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_LoadedTextures;
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_LoadedMeshes;

    /**
     * @brief 获取或加载指定路径的纹理（内部去重）。
     * @return 纹理指针，失败返回 nullptr
     */
    Texture *GetOrLoadTexture(const std::string &filepath);

    /**
     * @brief 获取或加载指定路径的网格（内部去重）。
     * @return 网格指针，失败返回 nullptr
     */
    Mesh *GetOrLoadMesh(const std::string &filepath);
};

} // namespace GE
