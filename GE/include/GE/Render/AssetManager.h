/**
 * @file AssetManager.h
 * @brief 统一资源管理器 —— 集中管理资源加载与资源路径。
 *
 * 作为纹理 / 材质 / 网格三个子管理器的门面（Facade），并提供：
 * - 可配置的资源根路径（默认 "assets"），支持统一重定位
 * - 统一的路径解析（ResolvePath），屏蔽硬编码相对路径
 * - 便捷加载入口（LoadTexture / LoadMesh），自动解析路径后委托子管理器
 *
 * 使用方式：
 * @code
 *   auto &am = Renderer::GetAssetManager();
 *   am.SetAssetRoot("assets");                                   // 设置资源根
 *   Texture* tex = am.LoadTexture("textures/foo.png");           // 解析 + 加载
 *   Mesh*    mesh = am.LoadMesh("meshes/foo.obj");
 *   Texture* tex2 = am.GetTextureManager().Load("textures/bar.png"); // 直接访问子管理器
 * @endcode
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace GE {

class VulkanDevice;
class VulkanResourceCache;
class TextureManager;
class MeshManager;
class MaterialManager;
class Texture;
class Mesh;

/**
 * @brief 集中定义的资源子路径常量（相对资源根目录）。
 *
 * 调用处可直接与文件名字符串拼接，例如 AssetPaths::Shaders "/sprite.vert.spv"
 * 得到 "shaders/glsl/sprite.vert.spv"，再交由 AssetManager::ResolvePath 解析。
 */
namespace AssetPaths {
    inline constexpr const char *Textures = "textures";       ///< 纹理目录
    inline constexpr const char *Models   = "models";         ///< 模型目录
    inline constexpr const char *Shaders  = "shaders/glsl";   ///< 着色器目录
    inline constexpr const char *Scenes   = "scenes";         ///< 场景目录
    inline constexpr const char *Fonts    = "fonts/opensans"; ///< 字体目录
    inline constexpr const char *HDRI     = "HDRI";           ///< HDR 环境贴图目录
}

/**
 * @brief 统一资源管理器。
 *
 * 持有纹理 / 网格 / 材质三个子管理器的所有权，并提供资源根路径配置与
 * 统一路径解析。生命周期与 Renderer 一致（由 Renderer 持有）。
 */
class AssetManager {
public:
    /**
     * @brief 构造资源管理器。
     *
     * @param device Vulkan 设备引用
     * @param cache  全局资源缓存引用
     */
    AssetManager(VulkanDevice &device, VulkanResourceCache &cache);

    ~AssetManager();

    AssetManager(const AssetManager &) = delete;
    AssetManager &operator=(const AssetManager &) = delete;
    AssetManager(AssetManager &&) = delete;
    AssetManager &operator=(AssetManager &&) = delete;

    // ========================================================================
    // 资源根路径
    // ========================================================================

    /**
     * @brief 设置资源根目录。
     *
     * 默认 "assets"。设置后所有相对路径解析都会基于该目录。
     *
     * @param root 资源根目录路径
     */
    void SetAssetRoot(const std::filesystem::path &root);

    /// 获取资源根目录。
    const std::filesystem::path &GetAssetRoot() const { return m_AssetRoot; }

    /**
     * @brief 统一路径解析。
     *
     * 规则：
     * - 空路径 → 返回空
     * - 以 "builtin:" 或 "solid:" 开头（内置几何体 / 纯色纹理伪键）→ 原样返回
     * - 绝对路径 → 规范化后原样返回
     * - 已带资源根前缀（如 root="assets"，入参 "assets/textures/foo.png"）→ 规范化后
     *   原样返回，避免重复拼接
     * - 其余相对路径 → 拼接到资源根目录后规范化返回
     *
     * @param path 原始路径
     * @return 解析后的路径
     */
    std::filesystem::path ResolvePath(const std::string &path) const;

    // ========================================================================
    // 子管理器访问
    // ========================================================================

    /// 访问纹理管理器。
    TextureManager &GetTextureManager();

    /// 访问网格管理器。
    MeshManager &GetMeshManager();

    /// 访问材质管理器。
    MaterialManager &GetMaterialManager();

    // ========================================================================
    // 便捷加载（自动解析路径后委托子管理器）
    // ========================================================================

    /**
     * @brief 加载纹理（自动解析路径）。
     *
     * 使用默认格式 eR8G8B8A8Unorm 与线性采样。
     *
     * @param path  纹理路径（相对资源根或绝对路径）
     * @return 纹理指针，加载失败返回 nullptr
     */
    Texture *LoadTexture(const std::string &path);

    /**
     * @brief 加载网格（自动解析路径）。
     *
     * @param path  网格路径（相对资源根或绝对路径）
     * @return 网格指针，加载失败返回 nullptr
     */
    Mesh *LoadMesh(const std::string &path);

private:
    /// 资源根目录，可通过 SetAssetRoot 重定位。
    std::filesystem::path m_AssetRoot{"assets"};

    /// 纹理管理器（拥有）。
    std::unique_ptr<TextureManager> m_TextureManager;

    /// 网格管理器（拥有）。
    std::unique_ptr<MeshManager> m_MeshManager;

    /// 材质管理器（拥有）。
    std::unique_ptr<MaterialManager> m_MaterialManager;
};

} // namespace GE