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
class AsyncUploadManager;
class TextureManager;
class MeshManager;
class MaterialManager;
class Texture;
class Mesh;
class Material;

namespace Audio {
    class SoundAsset;
    class SoundManager;
}

/**
 * @brief 集中定义的资源子路径常量（相对资源根目录）。
 *
 * 注意：该常量为 constexpr 变量而非字面量，不能直接与字符串字面量拼接。
 * 需用运行时拼接，例如 std::string(AssetPaths::Shaders) + "/sprite.vert.spv"
 * 得到 "shaders/glsl/sprite.vert.spv"，再交由 AssetManager::ResolvePath 解析。
 */
namespace AssetPaths {
    inline constexpr const char *Textures = "textures";       ///< 纹理目录
    inline constexpr const char *Models   = "models";         ///< 模型目录
    inline constexpr const char *Shaders  = "shaders/glsl";   ///< 着色器目录
    inline constexpr const char *Scenes   = "scenes";         ///< 场景目录
    inline constexpr const char *Scripts  = "scripts";        ///< Lua 脚本目录
    inline constexpr const char *Fonts    = "fonts/opensans"; ///< 字体目录
    inline constexpr const char *HDRI     = "HDRI";           ///< HDR 环境贴图目录
    inline constexpr const char *Audio    = "audio";          ///< 音频目录
    inline constexpr const char *Materials = "materials";      ///< 材质资产（.gemat）目录
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
     * @param upload 资源异步上传管理器引用（用于子管理器的异步加载）
     */
    AssetManager(VulkanDevice &device, VulkanResourceCache &cache, AsyncUploadManager &upload);

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
     * 默认 "assets"（相对启动时的工作目录）。应在**任何资产加载之前**调用；
     * Application 构造时已按"exe 同级 assets → 回退 CWD/assets"决策好并调用，
     * 发行版因此不依赖启动时的工作目录。
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
     * - 绝对路径且落在资源根之下 → 先相对化再拼接（历史场景的绝对路径自愈，
     *   换机器/换安装目录仍可解析）
     * - 绝对路径且落在资源根之外 → 打错误日志后原样返回（无法随包分发，
     *   由打包校验拦下；不静默失败）
     * - 相对路径 → 剥掉可能的资源根目录名前缀（如入参 "assets/textures/foo.png"）
     *   后拼接到资源根，避免重复拼接
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

    /// 访问声音资源管理器。
    Audio::SoundManager &GetSoundManager();

    /// 访问资源异步上传管理器（由 Renderer 持有，此处仅转发）。
    AsyncUploadManager &GetAsyncUploadManager();

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
     * @brief 异步加载纹理（自动解析路径）。
     *
     * 同 LoadTexture，但解码 + GPU 上传在后台线程，返回未就绪的空壳纹理，
     * 渲染端经 IsReady() 降级默认纹理，就绪后自动亮相。同路径只异步加载一次。
     *
     * @param path   纹理路径（相对资源根或绝对路径）
     * @param format 纹理格式。**颜色类贴图（albedo / emissive）必须显式传
     *               eR8G8B8A8Srgb**：TextureManager::LoadAsync 按**路径**缓存，
     *               同一张图被两个调用方以不同格式请求时，先到者的格式生效。
     *               默认 Unorm 只适合数据类贴图（法线 / 金属粗糙度）。
     * @return 纹理指针（未就绪的空壳），加载失败返回 nullptr
     */
    Texture *LoadTextureAsync(const std::string &path,
                              vk::Format format = vk::Format::eR8G8B8A8Unorm);

    /**
     * @brief 加载网格（自动解析路径）。
     *
     * 文件模型异步加载（后台解析 + GPU 上传），返回未就绪空壳（IsReady()=false），
     * 就绪后下帧自动可见；文件不存在返回 nullptr。内置几何体同步加载并立即就绪。
     *
     * @param path  网格路径（相对资源根或绝对路径）
     * @return 网格指针（文件模型为未就绪空壳），加载失败返回 nullptr
     */
    Mesh *LoadMesh(const std::string &path);

    /**
     * @brief 加载声音（自动解析路径）。
     *
     * @param path 声音路径（相对资源根或绝对路径）
     * @return 声音资源指针，加载失败返回 nullptr
     */
    Audio::SoundAsset *LoadSound(const std::string &path);

    /**
     * @brief 加载材质资产（`.gemat`，自动解析路径）。
     *
     * 同一文件只加载一次，之后复用同一材质实例（编辑器对它的改动不会被重读覆盖）。
     *
     * @param path 材质路径（相对资源根或绝对路径）
     * @return 材质指针，读取/解析失败返回 nullptr
     */
    Material *LoadMaterial(const std::string &path);

    /**
     * @brief 把材质写入 `.gemat` 文件（自动解析路径；目录不存在时自动创建）。
     *
     * 写成功后材质即"文件背书"，场景序列化对它写引用而非内联。
     *
     * @param mat  材质
     * @param path 目标路径（相对资源根或绝对路径）
     * @return 是否写入成功
     */
    bool SaveMaterial(Material &mat, const std::string &path);

    /**
     * @brief 保存所有有未保存改动的材质资产（场景保存时一并调用）。
     *
     * @return 实际写出的文件数
     */
    int SaveAllDirtyMaterials();

    /// 直接播放一次（编辑器试听 / 一次性 SFX），自动解析路径。
    bool PlayOneShot(const std::string &path, float volume = 1.0f);

private:
    /// 资源根目录，可通过 SetAssetRoot 重定位。
    std::filesystem::path m_AssetRoot{"assets"};

    /// 纹理管理器（拥有）。
    std::unique_ptr<TextureManager> m_TextureManager;

    /// 网格管理器（拥有）。
    std::unique_ptr<MeshManager> m_MeshManager;

    /// 材质管理器（拥有）。
    std::unique_ptr<MaterialManager> m_MaterialManager;

    /// 声音资源管理器（拥有；独立于 Vulkan）。
    std::unique_ptr<Audio::SoundManager> m_SoundManager;

    /// 异步上传管理器（非拥有，由 Renderer 持有），供子管理器异步加载使用。
    AsyncUploadManager *m_AsyncUpload = nullptr;
};

} // namespace GE
