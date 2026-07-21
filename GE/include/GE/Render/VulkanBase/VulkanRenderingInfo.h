#pragma once

#include <vulkan/vulkan.hpp>
#include <array>
#include <cstdint>

#include "Debug/Assert.h"

namespace GE {

class RenderTarget;  // 前置声明，用于 FromRenderTarget 工厂方法

/// Dynamic rendering 辅助类。
/// 直接维护 vk::RenderingInfo 成员变量，Begin() 时直接传递。
/// 使用固定数组替代 vector 避免每帧堆分配，最大支持 4 个 color attachment。
class VulkanRenderingInfo {
public:
    static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;

    VulkanRenderingInfo() = default;

    // ── 工厂方法 ────────────────────────────────────────────────

    /**
     * @brief 从 RenderTarget 构造 VulkanRenderingInfo。
     *
     * 自动配置所有附件：
     * - MSAA：多采样缓冲作为主附件，resolve 到 swapchain
     * - 非 MSAA：直接写入 swapchain 或离屏纹理
     * - 深度/模板：根据 RenderTarget 配置自动设置
     *
     * @param rt          渲染目标
     * @param renderArea  渲染区域（默认使用 rt.GetExtent()）
     * @return 配置好的 VulkanRenderingInfo
     */
    static VulkanRenderingInfo FromRenderTarget(const RenderTarget &rt,
                                                 const vk::Rect2D &renderArea);

    static VulkanRenderingInfo FromRenderTarget(const RenderTarget &rt);

    // ── Render Area ───────────────────────────────────────────────────

    void SetRenderArea(vk::Offset2D offset, vk::Extent2D extent) {
        m_RenderingInfo.renderArea = vk::Rect2D{offset, extent};
    }

    void SetRenderArea(int32_t x, int32_t y, uint32_t width, uint32_t height) {
        m_RenderingInfo.renderArea = vk::Rect2D{{x, y}, {width, height}};
    }

    void SetRenderArea(vk::Rect2D rect) { m_RenderingInfo.renderArea = rect; }

    [[nodiscard]] vk::Rect2D GetRenderArea() const { return m_RenderingInfo.renderArea; }

    // ── Layer / View Mask ─────────────────────────────────────────────

    void SetLayerCount(uint32_t count) { m_RenderingInfo.layerCount = count; }

    [[nodiscard]] uint32_t GetLayerCount() const { return m_RenderingInfo.layerCount; }

    void SetViewMask(uint32_t mask) { m_RenderingInfo.viewMask = mask; }

    [[nodiscard]] uint32_t GetViewMask() const { return m_RenderingInfo.viewMask; }

    // ── Color Attachments ─────────────────────────────────────────────

    void AddColorAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp,
                            vk::AttachmentStoreOp storeOp,
                            vk::ClearValue clearValue = {},
                            vk::ImageLayout layout = vk::ImageLayout::eColorAttachmentOptimal) {
        GE_CORE_ASSERT(m_ColorAttachmentCount < MAX_COLOR_ATTACHMENTS,
                       "Exceeded max color attachments (max 4)");
        m_ColorAttachments[m_ColorAttachmentCount] = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_ColorAttachmentCount++;
        m_RenderingInfo.colorAttachmentCount = m_ColorAttachmentCount;
        m_RenderingInfo.pColorAttachments = m_ColorAttachments.data();
    }

    /// 便捷重载：用纯色 clear（无需构造 vk::ClearValue）。
    void AddColorAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp,
                            vk::AttachmentStoreOp storeOp,
                            std::array<float, 4> clearColor,
                            vk::ImageLayout layout = vk::ImageLayout::eColorAttachmentOptimal) {
        vk::ClearValue cv;
        cv.color = clearColor;
        AddColorAttachment(imageView, loadOp, storeOp, cv, layout);
    }

    /// 带 resolve attachment 的 color attachment（MSAA → resolve）。
    void AddColorAttachmentWithResolve(vk::ImageView imageView,
                                       vk::ImageView resolveImageView,
                                       vk::AttachmentLoadOp loadOp,
                                       vk::AttachmentStoreOp storeOp,
                                       vk::ClearValue clearValue = {},
                                       vk::ImageLayout layout = vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::ImageLayout resolveLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::ResolveModeFlagBits resolveMode = vk::ResolveModeFlagBits::eAverage) {
        GE_CORE_ASSERT(m_ColorAttachmentCount < MAX_COLOR_ATTACHMENTS,
                       "Exceeded max color attachments (max 4)");
        m_ColorAttachments[m_ColorAttachmentCount] = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .resolveMode = resolveMode,
            .resolveImageView = resolveImageView,
            .resolveImageLayout = resolveLayout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_ColorAttachmentCount++;
        m_RenderingInfo.colorAttachmentCount = m_ColorAttachmentCount;
        m_RenderingInfo.pColorAttachments = m_ColorAttachments.data();
    }

    [[nodiscard]] uint32_t GetColorAttachmentCount() const { return m_ColorAttachmentCount; }

    void ClearColorAttachments() {
        m_ColorAttachmentCount = 0;
        m_RenderingInfo.colorAttachmentCount = 0;
        m_RenderingInfo.pColorAttachments = nullptr;
    }

    // ── Depth Attachment ──────────────────────────────────────────────

    void SetDepthAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear,
                            vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eDontCare,
                            vk::ImageLayout layout = vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        vk::ClearValue clearValue;
        clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
        m_DepthAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_RenderingInfo.pDepthAttachment = &m_DepthAttachment;
    }

    void SetDepthAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp,
                            vk::AttachmentStoreOp storeOp,
                            vk::ClearDepthStencilValue clearDepthStencil,
                            vk::ImageLayout layout = vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        vk::ClearValue clearValue;
        clearValue.depthStencil = clearDepthStencil;
        m_DepthAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_RenderingInfo.pDepthAttachment = &m_DepthAttachment;
    }

    /// 带 resolve attachment 的 depth attachment。
    void SetDepthAttachmentWithResolve(vk::ImageView imageView,
                                       vk::ImageView resolveImageView,
                                       vk::AttachmentLoadOp loadOp,
                                       vk::AttachmentStoreOp storeOp,
                                       vk::ClearDepthStencilValue clearDepthStencil = {1.0f, 0},
                                       vk::ImageLayout layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                       vk::ImageLayout resolveLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                       vk::ResolveModeFlagBits resolveMode = vk::ResolveModeFlagBits::eAverage) {
        vk::ClearValue clearValue;
        clearValue.depthStencil = clearDepthStencil;
        m_DepthAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .resolveMode = resolveMode,
            .resolveImageView = resolveImageView,
            .resolveImageLayout = resolveLayout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_RenderingInfo.pDepthAttachment = &m_DepthAttachment;
    }

    void ClearDepthAttachment() {
        m_RenderingInfo.pDepthAttachment = nullptr;
    }

    [[nodiscard]] bool HasDepthAttachment() const { return m_RenderingInfo.pDepthAttachment != nullptr; }

    // ── Stencil Attachment ────────────────────────────────────────────

    void SetStencilAttachment(vk::ImageView imageView,
                              vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear,
                              vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eDontCare,
                              uint32_t clearStencil = 0,
                              vk::ImageLayout layout = vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        vk::ClearValue clearValue;
        clearValue.depthStencil = vk::ClearDepthStencilValue{0.0f, clearStencil};
        m_StencilAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_RenderingInfo.pStencilAttachment = &m_StencilAttachment;
    }

    void ClearStencilAttachment() {
        m_RenderingInfo.pStencilAttachment = nullptr;
    }

    [[nodiscard]] bool HasStencilAttachment() const { return m_RenderingInfo.pStencilAttachment != nullptr; }

    // ── 共享 Depth/Stencil Attachment（同一 image view 同时作为 depth + stencil）──

    void SetDepthStencilAttachment(vk::ImageView imageView,
                                   vk::AttachmentLoadOp depthLoadOp = vk::AttachmentLoadOp::eClear,
                                   vk::AttachmentStoreOp depthStoreOp = vk::AttachmentStoreOp::eDontCare,
                                   vk::AttachmentLoadOp stencilLoadOp = vk::AttachmentLoadOp::eClear,
                                   vk::AttachmentStoreOp stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                                   vk::ClearDepthStencilValue clearValue = {1.0f, 0},
                                   vk::ImageLayout layout = vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        vk::ClearValue cv;
        cv.depthStencil = clearValue;
        m_DepthAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = depthLoadOp,
            .storeOp = depthStoreOp,
            .clearValue = cv,
        };
        m_StencilAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = stencilLoadOp,
            .storeOp = stencilStoreOp,
            .clearValue = cv,
        };
        m_RenderingInfo.pDepthAttachment = &m_DepthAttachment;
        m_RenderingInfo.pStencilAttachment = &m_StencilAttachment;
    }

    // ── Rendering 命令 ────────────────────────────────────────────────

    void Begin(vk::CommandBuffer cmd) const {
        cmd.beginRendering(m_RenderingInfo);
    }

    static void End(vk::CommandBuffer cmd) { cmd.endRendering(); }

    // ── 访问底层的 vk::RenderingInfo ─────────────────────────────────

    [[nodiscard]] const vk::RenderingInfo &GetRenderingInfo() const { return m_RenderingInfo; }
    [[nodiscard]] vk::RenderingInfo &GetRenderingInfo() { return m_RenderingInfo; }

    // ── 重置 / 重用 ───────────────────────────────────────────────────

    /// 重置所有 attachment 指针和计数器，允许对象重用。
    void Reset() {
        m_RenderingInfo = vk::RenderingInfo{};
        m_RenderingInfo.layerCount = 1;
        m_ColorAttachments = {};
        m_ColorAttachmentCount = 0;
        m_DepthAttachment = vk::RenderingAttachmentInfo{};
        m_StencilAttachment = vk::RenderingAttachmentInfo{};
    }

private:
    vk::RenderingInfo m_RenderingInfo{
        .layerCount = 1,
    };

    std::array<vk::RenderingAttachmentInfo, MAX_COLOR_ATTACHMENTS> m_ColorAttachments{};
    uint32_t m_ColorAttachmentCount = 0;

    vk::RenderingAttachmentInfo m_DepthAttachment{};
    vk::RenderingAttachmentInfo m_StencilAttachment{};
};

} // namespace GE