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

#include "Debug/Profiler.h"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace GE {

// ============================================================================
// 构造函数
// ============================================================================

VulkanRenderContext::VulkanRenderContext(VulkanDevice &device,
                                         vk::SurfaceKHR surface,
                                         const Window &window,
                                         const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list) : m_Device(device),
    m_AcquireSemaphorePool(device),

    m_Window(window),
    m_Queue(device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0)),
    m_SurfaceExtent{window.GetExtent().width, window.GetExtent().height} {
    // 从窗口的垂直同步配置推导出 Vulkan present mode 和优先级列表
    vk::PresentModeKHR present_mode;
    std::vector<vk::PresentModeKHR> present_mode_priority_list;

    switch (window.GetVSync()) {
    case VsyncMode::ON:
        present_mode = vk::PresentModeKHR::eFifo;
        present_mode_priority_list = {vk::PresentModeKHR::eFifo};
        break;
    case VsyncMode::OFF:
        // 优先 mailbox（无撕裂低延迟），immediate 次之，fifo 兜底
        present_mode = vk::PresentModeKHR::eMailbox;
        present_mode_priority_list = {
            vk::PresentModeKHR::eMailbox,
            vk::PresentModeKHR::eImmediate,
            vk::PresentModeKHR::eFifo};
        break;
    case VsyncMode::Default:
    default:
        // 默认：mailbox 优先，fallback fifo（兼顾低延迟和兼容性）
        present_mode = vk::PresentModeKHR::eMailbox;
        present_mode_priority_list = {
            vk::PresentModeKHR::eMailbox,
            vk::PresentModeKHR::eFifo};
        break;
    }

    InitializeSwapchain(surface, present_mode, present_mode_priority_list, surface_format_priority_list);
}

// ============================================================================
// 初始化 swapchain
// ============================================================================

void VulkanRenderContext::InitializeSwapchain(vk::SurfaceKHR surface,
                                              vk::PresentModeKHR present_mode,
                                              const std::vector<vk::PresentModeKHR> &present_mode_priority_list,
                                              const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list) {
    GE_PROFILE_FUNCTION();
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

VulkanCommandBuffer &VulkanRenderContext::Begin(CommandBufferResetMode reset_mode) {
    assert(m_Prepared && "VulkanRenderContext 未准备渲染，请先调用 Prepare()");

    if (!m_FrameActive) {
        BeginFrame();
    }

    if (m_Swapchain && !GetActiveFrame().HasAcquireSemaphore()) {
        throw std::runtime_error("无法开始帧：没有有效的 acquire semaphore");
    }

    const auto &queue = m_Device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    auto &cmd = GetActiveFrame().GetCommandPool(queue, reset_mode).RequestCommandBuffer();
    return cmd;
}

void VulkanRenderContext::BeginFrame() {
    GE_PROFILE_FUNCTION();

    // 处理待切换的呈现模式（在 acquire 之前重建 swapchain，避免当前帧 command buffer 引用旧 image）
    if (m_Swapchain && m_PendingPresentMode.has_value()) {
        auto new_mode = *m_PendingPresentMode;
        m_PendingPresentMode.reset();

        m_Device.GetHandle().waitIdle();
        m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, new_mode);
        Recreate();

        GE_CORE_INFO("Present mode updated");
    }

    // 仅在存在 swapchain 时处理 surface 变化
    if (m_Swapchain) {
        HandleSurfaceChanges();
    }

    assert(!m_FrameActive && "帧仍处于活跃状态，请先调用 Present");

    auto &prev_frame = *m_Frames[m_ActiveFrameIndex];

    // 2. 从全局 acquire semaphore 池申请一个 semaphore（上一帧刚归还的那个，空闲）
    VulkanSemaphore acquire_sem = m_AcquireSemaphorePool.RequestSemaphoreWithOwnership("AcquireSemaphore");

    if (m_Swapchain) {
        vk::Result result;
        try {
            std::tie(result, m_ActiveFrameIndex) = m_Swapchain->AcquireNextImage(acquire_sem.GetHandle());
        } catch (vk::OutOfDateKHRError & /*err*/) {
            result = vk::Result::eErrorOutOfDateKHR;
        }

        if (result == vk::Result::eSuboptimalKHR || result == vk::Result::eErrorOutOfDateKHR) {
            bool swapchain_updated = HandleSurfaceChanges(result == vk::Result::eErrorOutOfDateKHR);

            if (swapchain_updated) {
                // swapchain 重建后，旧 semaphore 可能已被 signal，销毁后重新申请
                m_AcquireSemaphorePool.ReleaseOwnedSemaphore(std::move(acquire_sem));
                acquire_sem = m_AcquireSemaphorePool.RequestSemaphoreWithOwnership("AcquireSemaphore");
                std::tie(result, m_ActiveFrameIndex) = m_Swapchain->AcquireNextImage(acquire_sem.GetHandle());
            }
        }

        if (result != vk::Result::eSuccess) {
            // 归还已申请的 acquire semaphore，防止泄漏
            m_AcquireSemaphorePool.ReleaseOwnedSemaphore(std::move(acquire_sem));
            return;
        }
    }

    // 3. 重置新的活跃帧（等待上一轮完成 + 归还旧的 acquire sem + 重置资源）
    auto &active_frame = *m_Frames[m_ActiveFrameIndex];
    ResetFrame(active_frame);

    // 4. 将新的 acquire semaphore 交给当前活跃帧持有
    active_frame.SetAcquireSemaphore(std::move(acquire_sem));

    // 标记帧为活跃
    m_FrameActive = true;
}

void VulkanRenderContext::Present(vk::Semaphore semaphore) {
    GE_PROFILE_FUNCTION();
    assert(m_FrameActive && "帧未激活，请先调用 BeginFrame");

    if (m_Swapchain) {
        auto vk_swapchain = m_Swapchain->GetHandle();

        vk::PresentInfoKHR present_info{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &semaphore,
            .swapchainCount = 1,
            .pSwapchains = &vk_swapchain,
            .pImageIndices = &m_ActiveFrameIndex,
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

    // 帧不再活跃（acquire semaphore 由当前帧持有，下次 Reset 时归还到全局池）
    m_FrameActive = false;
}

// ============================================================================
// 帧管理
// ============================================================================

VulkanSemaphore VulkanRenderContext::ConsumeAcquiredSemaphore() {
    assert(m_FrameActive && "帧未激活，请先调用 BeginFrame");
    return GetActiveFrame().TakeAcquireSemaphore();
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
    assert(!m_FrameActive && "帧仍处于活跃状态，请先调用 Present");
    return *m_Frames[m_ActiveFrameIndex];
}

std::vector<std::unique_ptr<VulkanRenderFrame> > &VulkanRenderContext::GetRenderFrames() {
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
    GE_PROFILE_FUNCTION();
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

        UpdateSwapchain(surface_properties.currentExtent, m_PreTransform);

        m_SurfaceExtent = surface_properties.currentExtent;

        return true;
    }

    return false;
}

// ============================================================================
// 准备与重建
// ============================================================================

void VulkanRenderContext::Prepare(size_t thread_count, bool enable_depth) {
    GE_PROFILE_FUNCTION();
    m_Device.GetHandle().waitIdle();

    if (m_Swapchain) {
        m_SurfaceExtent = m_Swapchain->GetExtent();

        vk::Extent3D extent{m_SurfaceExtent.width, m_SurfaceExtent.height, 1};

        // 为每个 swapchain image 创建 RenderTarget 和 RenderFrame
        RenderTargetDesc desc;
        desc.extent = m_SurfaceExtent;
        desc.colorFormat = m_Swapchain->GetFormat();
        desc.sampleCount = vk::SampleCountFlagBits::e1;
        desc.enableMSAA = false;
        desc.enableDepth = enable_depth;

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
    m_EnableDepth = enable_depth;
    m_Prepared = true;
}

void VulkanRenderContext::Recreate() {
    GE_PROFILE_FUNCTION();
    vk::Extent2D swapchain_extent = m_Swapchain->GetExtent();
    vk::Extent3D extent{swapchain_extent.width, swapchain_extent.height, 1};

    auto frame_it = m_Frames.begin();

    RenderTargetDesc desc;
    desc.extent = swapchain_extent;
    desc.colorFormat = m_Swapchain->GetFormat();
    desc.sampleCount = vk::SampleCountFlagBits::e1;
    desc.enableMSAA = false;
    desc.enableDepth = m_EnableDepth;

    for (auto &image_handle : m_Swapchain->GetImages()) {
        auto image_view = std::make_unique<VulkanImageView>(image_handle, vk::ImageViewType::e2D, m_Swapchain->GetFormat());
        auto render_target = std::make_unique<RenderTarget>(m_Device, desc, std::move(image_view));

        if (frame_it != m_Frames.end()) {
            (*frame_it)->UpdateRenderTarget(std::move(render_target));
        } else {
            // 若新 swapchain 的 image 数量多于当前帧数，创建新帧
            m_Frames.emplace_back(std::make_unique<VulkanRenderFrame>(m_Device, std::move(render_target), m_ThreadCount));
        }

        ++frame_it;
    }
}


// ============================================================================
// Swapchain 更新
// ============================================================================

void VulkanRenderContext::UpdateSwapchain(const vk::Extent2D &extent) {
    GE_PROFILE_FUNCTION();
    if (!m_Swapchain) {
        return;
    }
    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, extent);
    Recreate();
}

void VulkanRenderContext::UpdateSwapchain(uint32_t image_count) {
    GE_PROFILE_FUNCTION();
    if (!m_Swapchain) {
        return;
    }

    m_Device.GetHandle().waitIdle();

    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, image_count);

    Recreate();
}

void VulkanRenderContext::UpdateSwapchain(const std::set<vk::ImageUsageFlagBits> &image_usage_flags) {
    GE_PROFILE_FUNCTION();
    if (!m_Swapchain) {
        return;
    }

    m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Swapchain, image_usage_flags);

    Recreate();
}

void VulkanRenderContext::UpdateSwapchain(const vk::Extent2D &extent, vk::SurfaceTransformFlagBitsKHR transform) {
    GE_PROFILE_FUNCTION();
    if (!m_Swapchain) {
        return;
    }

    auto width = extent.width;
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
// 请求切换 present mode（延迟到下一帧开始时执行，保证当前帧 command buffer 安全）
// ============================================================================

void VulkanRenderContext::UpdateSwapchain(vk::PresentModeKHR present_mode) {
    GE_PROFILE_FUNCTION();
    if (!m_Swapchain) {
        return;
    }

    m_PendingPresentMode = present_mode;
}

// ============================================================================
// Semaphore 辅助
// ============================================================================

void VulkanRenderContext::ReleaseOwnedSemaphore(VulkanSemaphore semaphore) {
    GetActiveFrame().GetSemaphorePool().ReleaseOwnedSemaphore(std::move(semaphore));
}

vk::Semaphore VulkanRenderContext::RequestSemaphore() {
    return GetActiveFrame().GetSemaphorePool().RequestSemaphore();
}

VulkanSemaphore VulkanRenderContext::RequestSemaphoreWithOwnership() {
    return GetActiveFrame().GetSemaphorePool().RequestSemaphoreWithOwnership();
}

// ============================================================================
// EndFrame — 提交 + present + 结束帧
// ============================================================================

void VulkanRenderContext::EndFrame(vk::CommandBuffer command_buffer) {
    std::vector<vk::CommandBuffer> command_buffers(1, command_buffer);
    EndFrame(command_buffers);
}

void VulkanRenderContext::EndFrame(const std::vector<vk::CommandBuffer> &command_buffers) {
    assert(m_FrameActive && "RenderContext 未激活，无法提交 command buffer。请先调用 Begin()");

    vk::Semaphore render_semaphore = nullptr;

    if (m_Swapchain) {
        auto &active_frame = GetActiveFrame();
        assert(active_frame.HasAcquireSemaphore() && "没有 acquired_semaphore，可能已被 consume？");
        render_semaphore = Submit(m_Queue, command_buffers, active_frame.GetAcquireSemaphore().GetHandle(),
                                  vk::PipelineStageFlagBits::eColorAttachmentOutput);
    }

    Present(render_semaphore);
}

vk::Semaphore VulkanRenderContext::Submit(const VulkanQueue &queue,
                                          const std::vector<vk::CommandBuffer> &command_buffers,
                                          vk::Semaphore wait_semaphore,
                                          vk::PipelineStageFlags wait_pipeline_stage) {
    GE_PROFILE_FUNCTION();
    VulkanRenderFrame &frame = *m_Frames[m_ActiveFrameIndex];

    vk::Semaphore signal_semaphore = frame.GetSemaphorePool().RequestSemaphore("SignalSemaphore");

    vk::SubmitInfo submit_info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait_semaphore,
        .pWaitDstStageMask = &wait_pipeline_stage,
        .commandBufferCount = static_cast<uint32_t>(command_buffers.size()),
        .pCommandBuffers = command_buffers.data(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal_semaphore,
    };

    vk::Fence fence = frame.GetFencePool().RequestFence();
    queue.GetHandle().submit(submit_info, fence);

    return signal_semaphore;
}


// ============================================================================
// 等待帧
// ============================================================================

void VulkanRenderContext::WaitFrame() {
    GE_PROFILE_FUNCTION();
    ResetFrame(GetActiveFrame());
}

void VulkanRenderContext::ResetFrame(VulkanRenderFrame &frame) {
    // 先 Reset 帧：等待 fence + 重置 command/buffer/semaphore 等资源池
    frame.Reset();
    // fence 已完成，acquire semaphore 上的 GPU 操作肯定都结束了，安全归还到全局池
    if (frame.HasAcquireSemaphore()) {
        m_AcquireSemaphorePool.ReleaseOwnedSemaphore(frame.TakeAcquireSemaphore());
    }
}

// ============================================================================
// 析构
// ============================================================================

VulkanRenderContext::~VulkanRenderContext() {
    GE_PROFILE_FUNCTION();
    if (m_FrameActive) {
        m_FrameActive = false;
    }
    // 所有 frame 析构时会自动释放各自持有的 acquire semaphore
    // 全局 m_AcquireSemaphorePool 析构时会自动释放池内的 semaphore
    m_Frames.clear();
    m_Swapchain.reset();
}

} // namespace GE