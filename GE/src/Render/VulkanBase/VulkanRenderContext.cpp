/* Copyright (c) 2019-2026, Arm Limited and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file VulkanRenderContext.cpp
 * @brief 帧管理器实现，适配自 Vulkan-Samples 的 RenderContext。
 */

#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanImage.h"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace GE {

// ============================================================================
// 构造函数
// ============================================================================

VulkanRenderContext::VulkanRenderContext(VulkanDevice                           &device,
                                         vk::SurfaceKHR                          surface,
                                         const Window                           &window,
                                         vk::PresentModeKHR                      present_mode,
                                         const std::vector<vk::PresentModeKHR>  &present_mode_priority_list,
                                         const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list) :
    m_Device(device),
    m_Window(window),
    m_Queue(device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0)),
    m_SurfaceExtent{window.GetExtent().width, window.GetExtent().height} {
    InitializeSwapchain(surface, present_mode, present_mode_priority_list, surface_format_priority_list);
}

// ============================================================================
// 初始化 swapchain
// ============================================================================

void VulkanRenderContext::InitializeSwapchain(vk::SurfaceKHR                           surface,
                                               vk::PresentModeKHR                       present_mode,
                                               const std::vector<vk::PresentModeKHR>   &present_mode_priority_list,
                                               const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list) {
    if (surface) {
        vk::SurfaceCapabilitiesKHR surface_properties = m_Device.GetGpu().GetHandle().getSurfaceCapabilitiesKHR(surface);

        if (surface_properties.currentExtent.width == 0xFFFFFFFF) {
            m_Swapchain = std::make_unique<VulkanSwapchain>(m_Device, surface, present_mode,
                                                             present_mode_priority_list, surface_format_priority_list,
                                                             m_SurfaceExtent);
        } else {
            m_Swapchain = std::make_unique<VulkanSwapchain>(m_Device, surface, present_mode,
                                                             present_mode_priority_list, surface_format_priority_list);
        }
    }
}

// ============================================================================
// 帧循环
// ============================================================================

std::shared_ptr<VulkanCommandBuffer> VulkanRenderContext::Begin(CommandBufferResetMode reset_mode) {
    assert(m_Prepared && "VulkanRenderContext 未准备渲染，请先调用 Prepare()");

    if (!m_FrameActive) {
        BeginFrame();
    }

    if (!m_AcquiredSemaphore) {
        throw std::runtime_error("无法开始帧：没有有效的 acquire semaphore");
    }

    const auto &queue = m_Device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    auto cmd = GetActiveFrame().GetCommandPool(queue, reset_mode).RequestCommandBuffer();
    return cmd;
}

void VulkanRenderContext::BeginFrame() {
    // 仅在存在 swapchain 时处理 surface 变化
    if (m_Swapchain) {
        HandleSurfaceChanges();
    }

    assert(!m_FrameActive && "帧仍处于活跃状态，请先调用 EndFrame");

    auto &prev_frame = *m_Frames[m_ActiveFrameIndex];

    // 获取 acquire semaphore（所有权转移，供不同帧上下文使用）
    m_AcquiredSemaphore = prev_frame.GetSemaphorePool().RequestSemaphoreWithOwnership();

    if (m_Swapchain) {
        vk::Result result;
        try {
            std::tie(result, m_ActiveFrameIndex) = m_Swapchain->AcquireNextImage(m_AcquiredSemaphore);
        } catch (vk::OutOfDateKHRError & /*err*/) {
            result = vk::Result::eErrorOutOfDateKHR;
        }

        if (result == vk::Result::eSuboptimalKHR || result == vk::Result::eErrorOutOfDateKHR) {
            bool swapchain_updated = HandleSurfaceChanges(result == vk::Result::eErrorOutOfDateKHR);

            if (swapchain_updated) {
                // 需要销毁并重新分配 acquired_semaphore，因为它可能已被 signal
                m_Device.GetHandle().destroySemaphore(m_AcquiredSemaphore);
                m_AcquiredSemaphore = prev_frame.GetSemaphorePool().RequestSemaphoreWithOwnership();
                std::tie(result, m_ActiveFrameIndex) = m_Swapchain->AcquireNextImage(m_AcquiredSemaphore);
            }
        }

        if (result != vk::Result::eSuccess) {
            prev_frame.Reset();
            return;
        }
    }

    // 标记帧为活跃
    m_FrameActive = true;

    // 等待上一轮渲染此帧的所有资源释放完毕
    WaitFrame();
}

void VulkanRenderContext::EndFrame(vk::Semaphore semaphore) {
    assert(m_FrameActive && "帧未激活，请先调用 BeginFrame");

    if (m_Swapchain) {
        auto vk_swapchain = m_Swapchain->GetHandle();

        vk::PresentInfoKHR present_info{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &semaphore,
            .swapchainCount     = 1,
            .pSwapchains        = &vk_swapchain,
            .pImageIndices      = &m_ActiveFrameIndex,
        };

        // 检查是否支持显示呈现信息
        vk::DisplayPresentInfoKHR disp_present_info;
        if (m_Device.GetGpu().IsExtensionSupported(VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME) &&
            m_Window.GetDisplayPresentInfo(reinterpret_cast<VkDisplayPresentInfoKHR *>(&disp_present_info),
                                           m_SurfaceExtent.width, m_SurfaceExtent.height)) {
            present_info.pNext = &disp_present_info;
        }

        vk::Result result;
        try {
            result = m_Queue.Present(present_info);
        } catch (vk::OutOfDateKHRError & /*err*/) {
            result = vk::Result::eErrorOutOfDateKHR;
        }

        if (result == vk::Result::eSuboptimalKHR || result == vk::Result::eErrorOutOfDateKHR) {
            HandleSurfaceChanges();
        }
    }

    // 帧不再活跃
    if (m_AcquiredSemaphore) {
        ReleaseOwnedSemaphore(m_AcquiredSemaphore);
        m_AcquiredSemaphore = nullptr;
    }
    m_FrameActive = false;
}

// ============================================================================
// 帧管理
// ============================================================================

vk::Semaphore VulkanRenderContext::ConsumeAcquiredSemaphore() {
    assert(m_FrameActive && "帧未激活，请先调用 BeginFrame");
    return std::exchange(m_AcquiredSemaphore, nullptr);
}

VulkanRenderFrame &VulkanRenderContext::GetActiveFrame() {
    assert(m_FrameActive && "帧未激活，请先调用 BeginFrame");
    return *m_Frames[m_ActiveFrameIndex];
}

uint32_t VulkanRenderContext::GetActiveFrameIndex() const {
    assert(m_FrameActive && "帧未激活，请先调用 BeginFrame");
    return m_ActiveFrameIndex;
}

VulkanRenderFrame &VulkanRenderContext::GetLastRenderedFrame() {
    assert(!m_FrameActive && "帧仍处于活跃状态，请先调用 EndFrame");
    return *m_Frames[m_ActiveFrameIndex];
}

std::vector<std::unique_ptr<VulkanRenderFrame>> &VulkanRenderContext::GetRenderFrames() {
    return m_Frames;
}

// ============================================================================
// 设备与表面
// ============================================================================

VulkanDevice &VulkanRenderContext::GetDevice() {
    return m_Device;
}

vk::Extent2D const &VulkanRenderContext::GetSurfaceExtent() const {
    return m_SurfaceExtent;
}

const VulkanSwapchain &VulkanRenderContext::GetSwapchain() const {
    assert(m_Swapchain && "Swapchain 无效");
    return *m_Swapchain;
}

VulkanSwapchain &VulkanRenderContext::GetSwapchain() {
    assert(m_Swapchain && "Swapchain 无效");
    return *m_Swapchain;
}

bool VulkanRenderContext::HasSwapchain() {
    return m_Swapchain != nullptr;
}

bool VulkanRenderContext::HandleSurfaceChanges(bool force_update) {
    if (!m_Swapchain) {
        // 离屏渲染，无 swapchain
        return false;
    }

    vk::SurfaceCapabilitiesKHR surface_properties = m_Device.GetGpu().GetHandle().getSurfaceCapabilitiesKHR(m_Swapchain->GetSurface());

    if (surface_properties.currentExtent.width == 0xFFFFFFFF) {
        return false;
    }

    // 仅在尺寸变化时重建 swapchain
    if (surface_properties.currentExtent.width != m_SurfaceExtent.width ||
        surface_properties.currentExtent.height != m_SurfaceExtent.height ||
        force_update) {
        // 重建 swapchain
        m_Device.GetHandle().waitIdle();

        UpdateSwapchainImpl(surface_properties.currentExtent, m_PreTransform);

        m_SurfaceExtent = surface_properties.currentExtent;

        return true;
    }

    return false;
}

// ============================================================================
// 准备与重建
// ============================================================================

void VulkanRenderContext::Prepare(size_t thread_count) {
    m_Device.GetHandle().waitIdle();

    if (m_Swapchain) {
        m_SurfaceExtent = m_Swapchain->GetExtent();

        vk::Extent3D extent{m_SurfaceExtent.width, m_SurfaceExtent.height, 1};

        // 为每个 swapchain image 创建 RenderTarget 和 RenderFrame
        RenderTargetDesc desc;
        desc.extent      = m_SurfaceExtent;
        desc.colorFormat = m_Swapchain->GetFormat();
        desc.sampleCount = vk::SampleCountFlagBits::e1;
        desc.enableMSAA  = false;
        desc.enableDepth = false;

        for (auto &image_handle : m_Swapchain->GetImages()) {
            // 创建 swapchain image 的 view 并移交所有权给 RenderTarget
            auto image_view = std::make_unique<VulkanImageView>(image_handle, vk::ImageViewType::e2D, m_Swapchain->GetFormat());

            // 创建 RenderTarget（取得 image_view 所有权）
            auto render_target = std::make_unique<RenderTarget>(m_Device, desc, std::move(image_view));

            // 创建 RenderFrame
            m_Frames.emplace_back(std::make_unique<VulkanRenderFrame>(m_Device, std::move(render_target), thread_count));
        }
    }
    // TODO: 离屏渲染模式（无 swapchain）

    m_ThreadCount = thread_count;
    m_Prepared    = true;
}

void VulkanRenderContext::Recreate() {
    vk::Extent2D swapchain_extent = m_Swapchain->GetExtent();
    vk::Extent3D extent{swapchain_extent.width, swapchain_extent.height, 1};

    auto frame_it = m_Frames.begin();

    RenderTargetDesc desc;
    desc.extent      = swapchain_extent;
    desc.colorFormat = m_Swapchain->GetFormat();
    desc.sampleCount = vk::SampleCountFlagBits::e1;
    desc.enableMSAA  = false;
    desc.enableDepth = false;

    for (auto &image_handle : m_Swapchain->GetImages()) {
        auto image_view   = std::make_unique<VulkanImageView>(image_handle, vk::ImageViewType::e2D, m_Swapchain->GetFormat());
        auto render_target = std::make_unique<RenderTarget>(m_Device, desc, std::move(image_view));

        if (frame_it != m_Frames.end()) {
            (*frame_it)->UpdateRenderTarget(std::move(render_target));
        } else {
            // 若新 swapchain 的 image 数量多于当前帧数，创建新帧
            m_Frames.emplace_back(std::make_unique<VulkanRenderFrame>(m_Device, std::move(render_target), m_ThreadCount));
        }

        ++frame_it;
    }

    // 清除 framebuffer 缓存（ResourceCaching）
    // 注意：当前项目未实现 ResourceCaching 的 framebuffer 缓存清除
    // 若后续实现，在此处添加
}

void VulkanRenderContext::RecreateSwapchain() {
    m_Device.GetHandle().waitIdle();

    // 清除 framebuffer 缓存
    // 注意：当前项目未实现 ResourceCaching 的 framebuffer 缓存清除
    // 若后续实现，在此处添加

    vk::Extent2D swapchain_extent = m_Swapchain->GetExtent();
    vk::Extent3D extent{swapchain_extent.width, swapchain_extent.height, 1};

    RenderTargetDesc desc;
    desc.extent      = swapchain_extent;
    desc.colorFormat = m_Swapchain->GetFormat();
    desc.sampleCount = vk::SampleCountFlagBits::e1;
    desc.enableMSAA  = false;
    desc.enableDepth = false;

    auto frame_it = m_Frames.begin();

    for (auto &image_handle : m_Swapchain->GetImages()) {
        auto image_view   = std::make_unique<VulkanImageView>(image_handle, vk::ImageViewType::e2D, m_Swapchain->GetFormat());
        auto render_target = std::make_unique<RenderTarget>(m_Device, desc, std::move(image_view));

        (*frame_it)->UpdateRenderTarget(std::move(render_target));
        ++frame_it;
    }
}

// ============================================================================
// Swapchain 更新
// ============================================================================

void VulkanRenderContext::UpdateSwapchain(const vk::Extent2D &extent) {
    if (!m_Swapchain) {
        return;
    }

    // 清除 framebuffer 缓存
    // 注意：当前项目未实现 ResourceCaching 的 framebuffer 缓存清除

    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, extent);

    Recreate();
}

void VulkanRenderContext::UpdateSwapchain(uint32_t image_count) {
    if (!m_Swapchain) {
        return;
    }

    // 清除 framebuffer 缓存
    // 注意：当前项目未实现 ResourceCaching 的 framebuffer 缓存清除

    m_Device.GetHandle().waitIdle();

    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, image_count);

    Recreate();
}

void VulkanRenderContext::UpdateSwapchain(const std::set<vk::ImageUsageFlagBits> &image_usage_flags) {
    if (!m_Swapchain) {
        return;
    }

    // 清除 framebuffer 缓存
    // 注意：当前项目未实现 ResourceCaching 的 framebuffer 缓存清除

    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, image_usage_flags);

    Recreate();
}

void VulkanRenderContext::UpdateSwapchain(const vk::Extent2D &extent, vk::SurfaceTransformFlagBitsKHR transform) {
    UpdateSwapchainImpl(extent, transform);
}

void VulkanRenderContext::UpdateSwapchainImpl(const vk::Extent2D &extent, vk::SurfaceTransformFlagBitsKHR transform) {
    if (!m_Swapchain) {
        return;
    }

    // 清除 framebuffer 缓存
    // 注意：当前项目未实现 ResourceCaching 的 framebuffer 缓存清除

    auto width  = extent.width;
    auto height = extent.height;
    if (transform == vk::SurfaceTransformFlagBitsKHR::eRotate90 || transform == vk::SurfaceTransformFlagBitsKHR::eRotate270) {
        // 预旋转：始终使用原生方向
        std::swap(width, height);
    }

    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, vk::Extent2D{width, height}, transform);

    // 保存 preTransform 属性供后续旋转使用
    m_PreTransform = transform;

    Recreate();
}

// ============================================================================
// Semaphore 辅助
// ============================================================================

void VulkanRenderContext::ReleaseOwnedSemaphore(vk::Semaphore semaphore) {
    GetActiveFrame().GetSemaphorePool().ReleaseOwnedSemaphore(semaphore);
}

vk::Semaphore VulkanRenderContext::RequestSemaphore() {
    return GetActiveFrame().GetSemaphorePool().RequestSemaphore();
}

vk::Semaphore VulkanRenderContext::RequestSemaphoreWithOwnership() {
    return GetActiveFrame().GetSemaphorePool().RequestSemaphoreWithOwnership();
}

// ============================================================================
// 提交
// ============================================================================

void VulkanRenderContext::Submit(vk::CommandBuffer command_buffer) {
    std::vector<vk::CommandBuffer> command_buffers(1, command_buffer);
    Submit(command_buffers);
}

void VulkanRenderContext::Submit(const std::vector<vk::CommandBuffer> &command_buffers) {
    assert(m_FrameActive && "RenderContext 未激活，无法提交 command buffer。请先调用 Begin()");

    vk::Semaphore render_semaphore = nullptr;

    if (m_Swapchain) {
        assert(m_AcquiredSemaphore && "没有 acquired_semaphore，可能已被 consume？");
        render_semaphore = Submit(m_Queue, command_buffers, m_AcquiredSemaphore,
                                   vk::PipelineStageFlagBits::eColorAttachmentOutput);
    }

    EndFrame(render_semaphore);
}

vk::Semaphore VulkanRenderContext::Submit(const VulkanQueue           &queue,
                                           const std::vector<vk::CommandBuffer> &command_buffers,
                                           vk::Semaphore                wait_semaphore,
                                           vk::PipelineStageFlags       wait_pipeline_stage) {
    VulkanRenderFrame &frame = *m_Frames[m_ActiveFrameIndex];

    vk::Semaphore signal_semaphore = frame.GetSemaphorePool().RequestSemaphore();

    vk::SubmitInfo submit_info{
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &wait_semaphore,
        .pWaitDstStageMask    = &wait_pipeline_stage,
        .commandBufferCount   = static_cast<uint32_t>(command_buffers.size()),
        .pCommandBuffers      = command_buffers.data(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &signal_semaphore,
    };

    vk::Fence fence = frame.GetFencePool().RequestFence();
    queue.GetHandle().submit(submit_info, fence);

    return signal_semaphore;
}

void VulkanRenderContext::Submit(const VulkanQueue &queue, const std::vector<vk::CommandBuffer> &command_buffers) {
    vk::SubmitInfo submit_info{
        .commandBufferCount = static_cast<uint32_t>(command_buffers.size()),
        .pCommandBuffers    = command_buffers.data(),
    };

    vk::Fence fence = m_Frames[m_ActiveFrameIndex]->GetFencePool().RequestFence();
    queue.GetHandle().submit(submit_info, fence);
}

// ============================================================================
// 等待帧
// ============================================================================

void VulkanRenderContext::WaitFrame() {
    GetActiveFrame().Reset();
}

// ============================================================================
// 析构
// ============================================================================

VulkanRenderContext::~VulkanRenderContext() {
    // 确保帧已结束，释放 acquire semaphore（防止异常析构时泄漏）
    if (m_FrameActive) {
        m_FrameActive = false;
    }
    if (m_AcquiredSemaphore) {
        m_Device.GetHandle().destroySemaphore(m_AcquiredSemaphore);
        m_AcquiredSemaphore = nullptr;
    }

    // 显式清理 unique_ptr，确保析构顺序正确
    m_Frames.clear();      // 先销毁 RenderFrame（其内部资源可能引用 Device）
    m_Swapchain.reset();   // 再销毁 Swapchain
}

} // namespace GE