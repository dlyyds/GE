/**
 * @file MeshManager.h
 * @brief 全局网格管理器 —— 按路径加载、去重缓存。
 *
 * 统一管理所有网格资源的加载与生命周期：
 * - 按资源标识去重，同一网格只加载一次（跨场景共享）
 * - 支持内置几何体（builtin:cube / builtin:sphere 等）与外部模型文件
 * - 生命周期与 Renderer 一致（由 Renderer 持有），不随场景销毁
 *
 * 与 TextureManager / MaterialManager 对称：网格是无场景状态的纯 GPU 资源
 * （顶点 + 索引缓冲），本质上属于引擎级资产，而非某个场景。
 *
 * 使用方式：
 * @code
 *   auto& meshMgr = Renderer::GetMeshManager();
 *   Mesh* cube = meshMgr.GetBuiltin("cube");          // 内置几何体
 *   Mesh* model = Renderer::GetAssetManager().LoadMesh("models/foo.obj"); // 文件模型
 *   // 再次加载同路径，直接返回缓存
 *   assert(Renderer::GetAssetManager().LoadMesh("models/foo.obj") == model);
 * @endcode
 */

#pragma once

#include "Render/Mesh.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace GE {

class VulkanDevice;
class MaterialManager;
class TextureManager;

/**
 * @brief 全局网格管理器。
 *
 * 按资源标识（builtin:xxx 或文件路径）去重缓存，持有网格资源的所有权。
 * 需要 VulkanDevice 才能创建 / 加载网格。
 */
class MeshManager {
public:
    /**
     * @brief 构造网格管理器。
     * @param device          Vulkan 设备引用
     * @param materialManager 材质管理器引用（加载模型时创建子网格材质）
     * @param textureManager  纹理管理器引用（加载模型时按 MTL 加载材质纹理）
     */
    explicit MeshManager(VulkanDevice &device, MaterialManager &materialManager,
                         TextureManager &textureManager);

    ~MeshManager();

    MeshManager(const MeshManager &) = delete;
    MeshManager &operator=(const MeshManager &) = delete;
    MeshManager(MeshManager &&) = delete;
    MeshManager &operator=(MeshManager &&) = delete;

    // ========================================================================
    // 加载 / 获取
    // ========================================================================

    /**
     * @brief 加载网格（按资源标识去重，已加载则直接返回缓存）。
     *
     * 支持内置几何体路径（"builtin:cube" 等）与外部模型文件路径。
     *
     * @param filepath 内置标识或模型文件路径
     * @return 网格指针，加载失败返回 nullptr
     */
    Mesh *Load(const std::string &filepath);

    /**
     * @brief 获取已加载的网格（不触发加载）。
     *
     * @param filepath 内置标识或模型文件路径
     * @return 网格指针，未加载返回 nullptr
     */
    Mesh *Get(const std::string &filepath) const;

    /**
     * @brief 检查网格是否已加载。
     */
    bool Has(const std::string &filepath) const;

    /**
     * @brief 获取（或创建并缓存）一个内置几何体网格。
     *
     * 内部调用 Load("builtin:" + type)，按类型去重缓存。
     *
     * @param type 内置几何体类型（"cube" / "sphere" / "plane" / "quad"）
     * @return 网格指针，未知类型返回 nullptr
     */
    Mesh *GetBuiltin(const std::string &type);

    /**
     * @brief 手动注册一个网格到管理器中。
     *
     * 如果 key 已存在，旧网格会被替换并销毁。
     *
     * @param key  唯一标识键
     * @param mesh 网格指针（管理器接管所有权）
     * @return 注册后的网格指针
     */
    Mesh *Register(const std::string &key, std::unique_ptr<Mesh> mesh);

    /**
     * @brief 卸载指定网格。
     *
     * 注意：请确保没有任何组件 / 渲染器还在引用该网格指针。
     */
    void Unload(const std::string &filepath);

    /**
     * @brief 卸载所有网格。
     */
    void Clear();

    /**
     * @brief 获取已加载网格数量。
     */
    size_t GetCount() const { return m_Meshes.size(); }

    /**
     * @brief 获取所有已加载网格的标识键（builtin:xxx 或文件路径）。
     *
     * 用于编辑器 UI 下拉选择器等场景。
     */
    std::vector<std::string> GetAllKeys() const;

private:
    VulkanDevice   *m_Device   = nullptr;  ///< Vulkan 设备（不拥有）
    MaterialManager *m_Materials = nullptr; ///< 材质管理器（不拥有）
    TextureManager *m_Textures = nullptr;   ///< 纹理管理器（不拥有）

    /// 网格缓存：资源标识 -> mesh
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_Meshes;
};

} // namespace GE