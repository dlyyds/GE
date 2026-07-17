#pragma once

#include <vulkan/vulkan.hpp>
#include <array>
#include <cstdint>
#include <optional>

#include "Debug/Assert.h"

namespace GE {

/// Dynamic rendering 辅助类。
/// 封装 vk::RenderingInfo，用固定数组替代 vector 避免每帧堆分配。
/// 最大支持 4 个 color attachment（1 color + 3 resolve 的典型场景）。
class VulkanRenderingInfo {
public:
    static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;

    VulkanRenderingInfo() = default;

    // ── Render Area ───────────────────────────────────────────────────

    void SetRenderArea(vk::Offset2D offset, vk::Extent2D extent) {
        m_RenderArea = vk::Rect2D{offset, extent};
    }

    void SetRenderArea(int32_t x, int32_t y, uint32_t width, uint32_t height) {
        m_RenderArea = vk::Rect2D{{x, y}, {width, height}};
    }

    void SetRenderArea(vk::Rect2D rect) { m_RenderArea = rect; }

    [[nodiscard]] vk::Rect2D GetRenderArea() const { return m_RenderArea; }

    // ── Layer / View Mask ─────────────────────────────────────────────

    void SetLayerCount(uint32_t count) { m_LayerCount = count; }

    [[nodiscard]] uint32_t GetLayerCount() const { return m_LayerCount; }

    void SetViewMask(uint32_t mask) { m_ViewMask = mask; }

    [[nodiscard]] uint32_t GetViewMask() const { return m_ViewMask; }

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
                                       vk::AttachmentStoreOp resolveStoreOp,
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
    }

    [[nodiscard]] uint32_t GetColorAttachmentCount() const { return m_ColorAttachmentCount; }

    void ClearColorAttachments() { m_ColorAttachmentCount = 0; }

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
        m_HasDepth = true;
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
        m_HasDepth = true;
    }

    void SetDepthAttachmentWithResolve(vk::ImageView imageView,
                                       vk::ImageView resolveImageView,
                                       vk::AttachmentLoadOp loadOp,
                                       vk::AttachmentStoreOp storeOp,
                                       vk::AttachmentStoreOp resolveStoreOp,
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
        m_HasDepth = true;
    }

    void ClearDepthAttachment() { m_HasDepth = false; }

    [[nodiscard]] bool HasDepthAttachment() const { return m_HasDepth; }

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
        m_HasStencil = true;
    }

    void ClearStencilAttachment() { m_HasStencil = false; }

    [[nodiscard]] bool HasStencilAttachment() const { return m_HasStencil; }

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
        m_HasDepth   = true;
        m_HasStencil = true;
    }

    // ── Rendering 命令 ────────────────────────────────────────────────

    void Begin(vk::CommandBuffer cmd) const {
        vk::RenderingInfo info{
            .renderArea = m_RenderArea,
            .layerCount = m_LayerCount,
            .viewMask = m_ViewMask,
            .colorAttachmentCount = m_ColorAttachmentCount,
            .pColorAttachments = m_ColorAttachments.data(),
            .pDepthAttachment = m_HasDepth ? &m_DepthAttachment : nullptr,
            .pStencilAttachment = m_HasStencil ? &m_StencilAttachment : nullptr,
        };
        cmd.beginRendering(info);
    }

    static void End(vk::CommandBuffer cmd) { cmd.endRendering(); }

    // ── 重置 / 重用 ───────────────────────────────────────────────────

    /// 重置所有 attachment 计数器，允许对象重用。
    void Reset() {
        m_ColorAttachmentCount = 0;
        m_HasDepth = false;
        m_HasStencil = false;
    }

private:
    vk::Rect2D m_RenderArea{{0, 0}, {0, 0}};
    uint32_t m_LayerCount = 1;
    uint32_t m_ViewMask = 0;

    std::array<vk::RenderingAttachmentInfo, MAX_COLOR_ATTACHMENTS> m_ColorAttachments{};
    uint32_t m_ColorAttachmentCount = 0;

    vk::RenderingAttachmentInfo m_DepthAttachment{};
    bool m_HasDepth = false;

    vk::RenderingAttachmentInfo m_StencilAttachment{};
    bool m_HasStencil = false;
};

} // namespace GE
