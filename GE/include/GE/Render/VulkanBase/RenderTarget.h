/**
 * @file RenderTarget.h
 * @brief 渲染目标：单次渲染的附件集合，对应一次 Dynamic Rendering 的所有输出图像。
 *
 * 设计说明：
 *   - 封装一次渲染所需的所有附件资源（颜色、深度、MSAA resolve）
 *   - 与 VulkanRenderingInfo 配合：RenderTarget 持有资源，VulkanRenderingInfo 负责渲染配置
 *   - VulkanRenderingInfo 可通过 FromRenderTarget() 工厂方法从 RenderTarget 构造
 *   - 支持 MSAA：多采样颜色缓冲 + resolve 到交换链
 *   - 支持 MRT：最多 MAX_COLOR_ATTACHMENTS 个颜色附件
 *   - 支持重建（窗口 resize 时重建内部图像）
 *
 * 附件类型：
 *   - Swapchain 颜色附件：外部引用，不拥有所有权（由 Swapchain 管理）
 *   - MSAA 颜色缓冲：内部创建，多采样图像
 *   - 深度/模板附件：内部创建，可选择单采样或多采样
 *   - MRT 颜色附件：内部创建，离线纹理（未来扩展）
 */
#pragma once

#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanImageView.h"

#include <memory>
#include <vector>

namespace GE {

class VulkanDevice;
class VulkanSwapchain;

// ============================================================================
// RenderTarget 配置描述符
// ============================================================================

/**
 * @brief 描述 RenderTarget 的附件配置。
 *
 * 用于创建时一次性指定所有参数，避免构造函数参数过多。
 */
struct RenderTargetDesc {
    // --- 基础参数 ---
    vk::Extent2D extent{0, 0};                    ///< 渲染区域大小
    uint32_t     layerCount = 1;                  ///< 渲染层数（默认 1，数组/立方体贴图用）

    // --- 颜色附件 ---
    vk::Format colorFormat = vk::Format::eUndefined;  ///< 颜色格式（默认从 swapchain 推断）
    vk::ClearValue colorClearValue{};             ///< 颜色清除值

    // --- MSAA ---
    vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;  ///< 采样数（1 = 无 MSAA）
    bool enableMSAA = false;                      ///< 是否启用 MSAA

    // --- 深度/模板 ---
    bool enableDepth = false;                     ///< 是否启用深度测试
    bool enableStencil = false;                   ///< 是否启用模板测试
    vk::Format depthFormat = vk::Format::eUndefined;  ///< 深度格式（默认自动选择）
    vk::ClearValue depthClearValue{};             ///< 深度清除值（默认 1.0f, 0）

    // --- Load/Store 操作 ---
    vk::AttachmentLoadOp  colorLoadOp  = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp colorStoreOp = vk::AttachmentStoreOp::eStore;
    vk::AttachmentLoadOp  depthLoadOp  = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp depthStoreOp = vk::AttachmentStoreOp::eDontCare;
    vk::AttachmentLoadOp  stencilLoadOp  = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
};

// ============================================================================
// RenderTarget — 渲染目标
// ============================================================================

/**
 * @brief 单次渲染的附件集合。
 *
 * 持有并管理一次 Dynamic Rendering 调用所需的所有图像资源：
 * - 颜色附件（主颜色 + 可选 resolve 目标）
 * - 深度/模板附件
 * - MSAA 多采样缓冲
 *
 * 使用方式：
 * @code
 *   // 1. 创建描述符
 *   RenderTargetDesc desc;
 *   desc.extent = swapchain.GetExtent();
 *   desc.enableDepth = true;
 *   desc.sampleCount = vk::SampleCountFlagBits::e4;  // 4x MSAA
 *
 *   // 2. 创建 RenderTarget（绑定到 swapchain 的当前 image）
 *   RenderTarget rt(device, desc, swapchainImageView);
 *
 *   // 3. 通过 VulkanRenderingInfo 工厂方法构造并渲染
 *   VulkanRenderingInfo renderInfo = VulkanRenderingInfo::FromRenderTarget(rt);
 *   renderInfo.Begin(cmd);
 *   // ... 绘制 ...
 *   VulkanRenderingInfo::End(cmd);
 * @endcode
 */
class RenderTarget {
public:
    // --- 构造函数 ---

    /**
     * @brief 创建 RenderTarget（绑定到 swapchain image view）。
     * @param device        Vulkan 设备
     * @param desc          渲染目标配置
     * @param swapchainView 交换链当前帧的 VulkanImageView（外部引用，不拥有所有权）
     */
    RenderTarget(VulkanDevice &device,
                 const RenderTargetDesc &desc,
                 VulkanImageView &swapchainView);

    ~RenderTarget();

    RenderTarget(const RenderTarget &) = delete;
    RenderTarget(RenderTarget &&other) noexcept;
    RenderTarget &operator=(const RenderTarget &) = delete;
    RenderTarget &operator=(RenderTarget &&) = delete;

    [[nodiscard]] vk::Extent2D GetExtent() const { return m_Desc.extent; }
    [[nodiscard]] vk::Format   GetColorFormat() const { return m_Desc.colorFormat; }
    [[nodiscard]] vk::Format   GetDepthFormat() const { return m_Desc.depthFormat; }
    [[nodiscard]] bool         HasMSAA() const { return m_Desc.enableMSAA && m_Desc.sampleCount != vk::SampleCountFlagBits::e1; }
    [[nodiscard]] bool         HasDepth() const { return m_Desc.enableDepth; }
    [[nodiscard]] bool         HasStencil() const { return m_Desc.enableStencil; }
    [[nodiscard]] vk::SampleCountFlagBits GetSampleCount() const { return m_Desc.sampleCount; }
    [[nodiscard]] const RenderTargetDesc &GetDesc() const { return m_Desc; }

    // 封装对象访问（推荐使用）
    [[nodiscard]] bool HasSwapchainView() const { return true; }
    [[nodiscard]] VulkanImageView &GetSwapchainView() { return m_SwapchainView; }
    [[nodiscard]] const VulkanImageView &GetSwapchainView() const { return m_SwapchainView; }
    [[nodiscard]] VulkanImageView &GetColorResolveView();   ///< MSAA 时返回多采样缓冲的 view，非 MSAA 时返回 swapchainView
    [[nodiscard]] const VulkanImageView &GetColorResolveView() const;
    [[nodiscard]] VulkanImageView &GetDepthView();
    [[nodiscard]] const VulkanImageView &GetDepthView() const;

    // --- MRT 扩展（未来）---
    // void AddColorAttachment(...);
    // void RemoveColorAttachment(uint32_t index);

private:
    // --- 内部创建/销毁 ---

    void CreateResources();
    void DestroyResources();

    /// 创建 MSAA 颜色缓冲
    void CreateMSAAColorBuffer();
    /// 创建深度/模板缓冲
    void CreateDepthBuffer();

    /// 获取合适的深度格式
    vk::Format PickDepthFormat() const;

private:
    VulkanDevice &m_Device;
    RenderTargetDesc m_Desc;

    // --- 附件资源 ---

    // Swapchain 颜色附件（外部引用，不拥有所有权）
    VulkanImageView &m_SwapchainView;

    // MSAA 多采样颜色缓冲（内部创建）
    std::unique_ptr<VulkanImage>     m_MSAAColorImage;
    std::unique_ptr<VulkanImageView> m_MSAAColorView;

    // 深度/模板缓冲（内部创建）
    std::unique_ptr<VulkanImage>     m_DepthImage;
    std::unique_ptr<VulkanImageView> m_DepthView;

    // 深度 resolve 缓冲（MSAA 深度需要时创建，未来扩展）
    std::unique_ptr<VulkanImage>     m_DepthResolveImage;
    std::unique_ptr<VulkanImageView> m_DepthResolveView;
};

} // namespace GE
