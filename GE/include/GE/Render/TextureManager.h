/**
 * @file TextureManager.h
 * @brief 全局纹理管理器 —— 按路径加载、去重缓存。
 *
 * 统一管理所有纹理资源的加载与生命周期：
 * - 按文件路径去重，同一张纹理只加载一次
 * - 支持从文件加载（PNG / JPG 等 stb_image 格式）
 * - 支持手动注册已有纹理（用于程序化创建的纹理）
 * - 生命周期与 Renderer 一致（由 Renderer 持有）
 *
 * 使用方式：
 * @code
 *   // 推荐：经统一资源管理器自动解析路径
 *   auto& am = Renderer::GetAssetManager();
 *   Texture* tex = am.LoadTexture("textures/foo.png");
 *   // 或直接访问子管理器（需传入已解析路径）
 *   auto& texMgr = Renderer::GetTextureManager();
 *   Texture* tex2 = texMgr.Load("assets/textures/foo.png");
 *   // 再次加载同路径，直接返回缓存
 *   assert(tex == tex2);
 * @endcode
 */

#pragma once

#include "Render/Texture.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <memory>

namespace GE {

class VulkanDevice;
class VulkanResourceCache;
class AsyncUploadManager;

/**
 * @brief 全局纹理管理器。
 *
 * 按文件路径去重缓存，持有纹理资源的所有权。
 * 需要 VulkanDevice 和 VulkanResourceCache 才能加载纹理。
 */
class TextureManager {
public:
    /**
     * @brief 构造纹理管理器。
     *
     * @param device  Vulkan 设备引用
     * @param cache   全局资源缓存引用（用于 Sampler 去重）
     * @param upload  异步上传管理器引用（用于异步加载）
     */
    TextureManager(VulkanDevice &device, VulkanResourceCache &cache, AsyncUploadManager &upload);

    ~TextureManager();

    TextureManager(const TextureManager &) = delete;
    TextureManager &operator=(const TextureManager &) = delete;
    TextureManager(TextureManager &&) = delete;
    TextureManager &operator=(TextureManager &&) = delete;

    // ========================================================================
    // 加载 / 获取
    // ========================================================================

    /**
     * @brief 加载纹理（按路径去重，已加载则直接返回缓存）。
     *
     * @param filepath    纹理文件路径
     * @param format      纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter  放大过滤器（默认 eLinear）
     * @param min_filter  缩小过滤器（默认 eLinear）
     * @return 纹理指针，加载失败返回 nullptr
     *
     * @note 仅 filepath 参与去重，format/filter 只在首次加载时生效。
     *       如果同一路径需要不同采样参数的多份纹理，请使用不同的 key
     *       （或用 Register() 手动注册）。
     */
    Texture *Load(const std::string &filepath,
                  vk::Format format = vk::Format::eR8G8B8A8Unorm,
                  vk::Filter mag_filter = vk::Filter::eLinear,
                  vk::Filter min_filter = vk::Filter::eLinear);

    /**
     * @brief 异步加载纹理（按路径去重，已加载则直接返回缓存）。
     *
     * 与 Load() 不同：返回的是"空壳"纹理（未就绪），解码 + GPU 上传在后台线程，
     * 主线程每帧 Poll() 回收后自动注入并置就绪。渲染端须经 IsReady() 检查，
     * 未就绪时降级为默认纹理。
     *
     * @param filepath    纹理文件路径
     * @param format      纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter  放大过滤器（默认 eLinear）
     * @param min_filter  缩小过滤器（默认 eLinear）
     * @return 纹理指针（未就绪的空壳），加载失败返回 nullptr
     *
     * @note 同样仅 filepath 参与去重。未就绪条目也参与缓存查找，同路径只异步
     *       加载一次。unload 时若仍在异步加载中，内部 AsyncPendingSlot 自动作废，
     *       在途 finalize 会安全跳过注入，无 use-after-free。
     */
    Texture *LoadAsync(const std::string &filepath,
                       vk::Format format = vk::Format::eR8G8B8A8Unorm,
                       vk::Filter mag_filter = vk::Filter::eLinear,
                       vk::Filter min_filter = vk::Filter::eLinear);

    /**
     * @brief 获取已加载的纹理（不触发加载）。
     *
     * @param filepath  纹理文件路径
     * @return 纹理指针，未加载返回 nullptr
     */
    Texture *Get(const std::string &filepath) const;

    /**
     * @brief 检查纹理是否已加载。
     */
    bool Has(const std::string &filepath) const;

    /**
     * @brief 获取（或创建并缓存）一个纯色纹理。
     *
     * 按颜色去重缓存，同一颜色只创建一次。颜色分量会被量化到 8bit
     * （[0,1] → [0,255]），因此两个仅在低精度上不同的颜色会命中同一缓存。
     *
     * @param color      纯色 RGBA（分量 [0,1]）
     * @param format     纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter 放大过滤器（默认 eLinear）
     * @param min_filter 缩小过滤器（默认 eLinear）
     * @return 1x1 纯色纹理指针
     *
     * @note 返回的纹理由管理器持有，调用方不要 delete。
     */
    Texture *GetSolidColor(const glm::vec4 &color,
                           vk::Format format = vk::Format::eR8G8B8A8Unorm,
                           vk::Filter mag_filter = vk::Filter::eLinear,
                           vk::Filter min_filter = vk::Filter::eLinear);

    /**
     * @brief 手动注册一个纹理到管理器中。
     *
     * 用于注册程序化创建的、不从文件加载的纹理。
     * 如果 key 已存在，旧纹理会被替换并销毁。
     *
     * @param key     唯一标识键
     * @param texture 纹理指针（管理器接管所有权）
     * @return 注册后的纹理指针
     */
    Texture *Register(const std::string &key, std::unique_ptr<Texture> texture);

    /**
     * @brief 卸载指定纹理。
     *
     * 注意：请确保没有任何组件/材质还在引用该纹理指针。
     */
    void Unload(const std::string &filepath);

    /**
     * @brief 卸载所有纹理。
     */
    void Clear();

    /**
     * @brief 获取已加载纹理数量。
     */
    size_t GetCount() const { return m_Textures.size(); }

    /**
     * @brief 获取所有已加载纹理的键名（文件路径或自定义 key）。
     *
     * 用于编辑器 UI 下拉选择器等场景。
     */
    std::vector<std::string> GetAllKeys() const;

private:
    VulkanDevice        *m_Device  = nullptr;  ///< Vulkan 设备（不拥有）
    VulkanResourceCache *m_Cache   = nullptr;  ///< 全局资源缓存（不拥有）
    AsyncUploadManager  *m_AsyncUpload = nullptr; ///< 异步上传管理器（不拥有）

    /// 纹理缓存：文件路径 / 自定义 key -> texture
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_Textures;
};

} // namespace GE
