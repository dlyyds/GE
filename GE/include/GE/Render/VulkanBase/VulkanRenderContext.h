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
 * @file VulkanRenderContext.h
 * @brief 帧管理器，适配自 Vulkan-Samples 的 RenderContext。
 *
 * VulkanRenderContext 作为帧管理器，生命周期与 Application 相同。
 * 它是 RenderFrame 对象的容器，在帧之间切换（BeginFrame、Present）
 * 并将 Vulkan 资源的请求转发给当前活跃帧。
 *
 * 对于使用 swapchain 的正常渲染，传入 swapchain 创建参数，
 * 每个 swapchain image 对应一个 RenderFrame。
 *
 * 对于离屏渲染（无 swapchain），传入有效设备和宽高，
 * 创建单个 RenderFrame。
 */

#pragma once

#include "Core/GEWindow.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanQueue.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanSemaphore.h"
#include "Render/VulkanBase/VulkanSwapchain.h"

#include <vulkan/vulkan.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace GE {

/**
 * @brief 帧管理器。
 *
 * RenderContext 作为帧管理器，生命周期与 Application 相同。
 * 它是 RenderFrame 对象的容器，在帧之间切换（BeginFrame、Present）
 * 并将 Vulkan 资源的请求转发给当前活跃帧。
 *
 * 保证始终存在一个活跃帧。
 * 同一时刻 GPU 中可能有多个帧在飞行，因此需要每帧独立的资源。
 */
class VulkanRenderContext {
public:
    // ========================================================================
    // 构造函数
    // ========================================================================

    /**
     * @brief 构造 RenderContext（带 swapchain 模式）。
     * @param device                      Vulkan 设备
     * @param surface                     呈现 surface
     * @param window                      创建 surface 的窗口
     * @param present_mode                请求的呈现模式
     * @param present_mode_priority_list  呈现模式优先级列表
     * @param surface_format_priority_list surface 格式优先级列表
     */
    VulkanRenderContext(VulkanDevice &device,
                        vk::SurfaceKHR surface,
                        const Window &window,
                        vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo,
                        const std::vector<vk::PresentModeKHR> &present_mode_priority_list = {vk::PresentModeKHR::eFifo, vk::PresentModeKHR::eMailbox},
                        const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list = {
                            {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
                            {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}});

    VulkanRenderContext(const VulkanRenderContext &) = delete;

    VulkanRenderContext(VulkanRenderContext &&) = delete;

    virtual ~VulkanRenderContext();

    VulkanRenderContext &operator=(const VulkanRenderContext &) = delete;

    VulkanRenderContext &operator=(VulkanRenderContext &&) = delete;

    // ========================================================================
    // 帧循环接口
    // ========================================================================

    /**
     * @brief 准备下一帧用于渲染。
     * @param reset_mode 如何重置 command buffer
     * @return 有效的 command buffer 引用，用于录制提交命令。
     *         同时确保当前帧已激活（若尚未激活）。
     *         生命周期跟随当前帧的 command pool。
     */
    VulkanCommandBuffer &Begin(CommandBufferResetMode reset_mode = CommandBufferResetMode::ResetPool);

    /**
     * @brief 开始新帧（acquire next image，处理 surface 变化）。
     */
    void BeginFrame();

    /**
     * @brief 提交单个 command buffer 并结束帧（提交 + present + 清理）。
     * @param command_buffer 包含录制命令的 command buffer
     */
    void EndFrame(vk::CommandBuffer command_buffer);

    /**
     * @brief 提交多个 command buffer 并结束帧（提交 + present + 清理）。
     * @param command_buffers 包含录制命令的 command buffer 列表
     */
    void EndFrame(const std::vector<vk::CommandBuffer> &command_buffers);

    /**
     * @brief 呈现当前帧图像到 swapchain 并结束帧（内部使用，通常由 EndFrame 调用）。
     * @param semaphore 提交完成后 signal 的 semaphore，present 将等待它
     */
    void Present(vk::Semaphore semaphore);

    // ========================================================================
    // 帧管理
    // ========================================================================

    /**
     * @brief 获取并消费当前帧的 WSI acquire semaphore（所有权转移）。仅在特殊情况下使用。
     * @return WSI acquire semaphore（调用方拥有所有权）。
     */
    VulkanSemaphore ConsumeAcquiredSemaphore();

    /**
     * @brief 获取当前活跃帧。
     * @return 当前活跃帧的引用。
     *         帧在 begin_frame 调用后激活。
     */
    VulkanRenderFrame &GetActiveFrame();


    /**
     * @brief 获取当前活跃帧索引。
     * @return 当前活跃帧索引。
     */
    uint32_t GetActiveFrameIndex() const;

    /**
     * @brief 获取上一帧。
     * @return 上一帧的引用。
     *         帧在 Present 调用后变为上一帧。
     */
    VulkanRenderFrame &GetLastRenderedFrame();

    /**
     * @brief 获取所有 RenderFrame。
     * @return RenderFrame 列表。
     */
    std::vector<std::unique_ptr<VulkanRenderFrame> > &GetRenderFrames();

    // ========================================================================
    // 设备与表面
    // ========================================================================

    VulkanDevice &GetDevice();

    /**
     * @brief 获取 surface extent。
     */
    vk::Extent2D const &GetSurfaceExtent() const;

    /**
     * @brief 获取 swapchain 引用。
     */
    const VulkanSwapchain &GetSwapchain() const;

    /**
     * @brief 获取 swapchain 引用（非常量）。
     */
    VulkanSwapchain &GetSwapchain();

    /**
     * @brief 处理 surface 变化（仅在有 swapchain 时适用）。
     * @param force_update 是否强制更新
     * @return 是否发生了重建
     */
    virtual bool HandleSurfaceChanges(bool force_update = false);

    /**
     * @brief 是否有有效的 swapchain。
     */
    bool HasSwapchain();

    // ========================================================================
    // 准备与重建
    // ========================================================================

    /**
     * @brief 准备 RenderFrame 用于渲染。
     * @param thread_count  应用线程数，每个 RenderFrame 分配此数量的资源池
     * @param enable_depth  是否为每个 RenderTarget 创建深度缓冲
     */
    void Prepare(size_t thread_count = 1, bool enable_depth = false);

    /**
     * @brief 重建 RenderFrame（swapchain 重建后调用）。
     */
    void Recreate();

    // ========================================================================
    // Swapchain 更新
    // ========================================================================

    /**
     * @brief 更新 swapchain 的 extent。
     */
    void UpdateSwapchain(const vk::Extent2D &extent);

    /**
     * @brief 更新 swapchain 的 image count。
     */
    void UpdateSwapchain(uint32_t image_count);

    /**
     * @brief 更新 swapchain 的 image usage。
     */
    void UpdateSwapchain(const std::set<vk::ImageUsageFlagBits> &image_usage_flags);

    /**
     * @brief 更新 swapchain 的 extent 和 surface transform。
     */
    void UpdateSwapchain(const vk::Extent2D &extent, vk::SurfaceTransformFlagBitsKHR transform);

    // ========================================================================
    // Semaphore 辅助
    // ========================================================================

    void ReleaseOwnedSemaphore(VulkanSemaphore semaphore);

    vk::Semaphore RequestSemaphore();

    VulkanSemaphore RequestSemaphoreWithOwnership();

    // ========================================================================
    // 提交
    // ========================================================================

    /**
     * @brief 提交 command buffer 到指定队列，带等待 semaphore，不 present。
     * @param queue              目标队列
     * @param command_buffers    包含录制命令的 command buffer 列表
     * @param wait_semaphore     等待的 semaphore
     * @param wait_pipeline_stage 等待的 pipeline stage
     * @return submit 完成后 signal 的 semaphore
     */
    vk::Semaphore Submit(const VulkanQueue &queue,
                         const std::vector<vk::CommandBuffer> &command_buffers,
                         vk::Semaphore wait_semaphore,
                         vk::PipelineStageFlags wait_pipeline_stage);


    /**
     * @brief 等待当前帧完成渲染。
     */
    virtual void WaitFrame();

private:
    // ========================================================================
    // 内部实现
    // ========================================================================

    void InitializeSwapchain(vk::SurfaceKHR surface, vk::PresentModeKHR present_mode,
                             const std::vector<vk::PresentModeKHR> &present_mode_priority_list,
                             const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list);

    /**
     * @brief 重置一个 RenderFrame：归还其 acquire semaphore 到全局池，再重置帧资源。
     * @param frame 要重置的帧
     */
    void ResetFrame(VulkanRenderFrame &frame);

    // ========================================================================
    // 成员变量
    // ========================================================================

    uint32_t m_ActiveFrameIndex{0};
    VulkanDevice &m_Device;
    bool m_FrameActive{false};
    VulkanSemaphorePool m_AcquireSemaphorePool;  ///< 全局 acquire semaphore 池（所有 frame 共用）
    std::vector<std::unique_ptr<VulkanRenderFrame> > m_Frames;
    vk::SurfaceTransformFlagBitsKHR m_PreTransform{vk::SurfaceTransformFlagBitsKHR::eIdentity};
    bool m_Prepared{false};
    const VulkanQueue &m_Queue; // 若存在 swapchain，则为 present 支持的队列，否则为 graphics 队列
    vk::Extent2D m_SurfaceExtent{};
    std::unique_ptr<VulkanSwapchain> m_Swapchain;
    size_t m_ThreadCount{1};
    bool m_EnableDepth{false};  ///< RenderTarget 是否启用深度缓冲
    const Window &m_Window;
};

} // namespace GE