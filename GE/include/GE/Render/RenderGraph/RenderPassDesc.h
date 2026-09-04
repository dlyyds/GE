/**
 * @file RenderPassDesc.h
 * @brief 渲染图 Pass 声明（RenderPassDesc）与执行回调上下文（PassExecuteContext）。
 *
 * 一个 pass 描述一次图节点：它读哪些资源、写哪些资源、渲染区域/附件语义如何，
 * 以及该 pass 实际的命令录制回调。图只负责推导依赖、屏障与布局转换，实际
 * vkCmd* 命令全部在 execute 回调内录制。
 *
 * 依赖与同步由「资源读写声明」推导，因此回调内不应再出现任何手动屏障
 * （见计划书 §4.4 —— 这是规则而非约定）。
 */

#pragma once

#include "Render/RenderGraph/RenderGraphTypes.h"

#include <vulkan/vulkan.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GE {

class VulkanCommandBuffer;
class VulkanRenderFrame;
class VulkanImageView;
class VulkanRenderingInfo;

/// pass 执行回调收到的上下文（完整定义见下）。
struct PassExecuteContext;

/// 一个 pass 的完整声明。由 RenderGraphBuilder::AddPass 创建并填充。
struct RenderPassDesc {
    std::string name;                          ///< 调试名（RenderDoc / 日志 / 断言定位）
    PassType    type = PassType::Raster;       ///< pass 种类（v1 仅 Raster）

    // --- 附件（raster pass） ---
    std::vector<AttachmentDesc> colorAttachments;   ///< 颜色附件（可多个，动态渲染支持）
    std::optional<AttachmentDesc> depthAttachment;  ///< 深度附件（可选）

    vk::Rect2D renderArea{};                   ///< 渲染区域；无效区（empty）时执行期用颜色附件 extent 填充

    // --- 输入与资源读写（所有 pass 共用） ---
    std::vector<ImageInputDesc> readImages;    ///< 采样/只读输入（ShaderRead）
    // writeImages 由附件描述 + 预留字段表达；v1 附加在 colorAttachments/depthAttachment 的资源上。

    /// 透传给 execute 的用户数据（渲染器/调用方持有，图不解释）。
    void *userData = nullptr;

    /// 命令录制回调：图在已转好布局、已打开动态渲染（raster）后调用。
    /// 见 PassExecuteContext。
    std::function<void(PassExecuteContext &)> execute;
};

// ============================================================================
// PassExecuteContext —— 执行期传给回调的上下文
// ============================================================================

/**
 * @brief pass 执行回调收到的上下文。
 *
 * 图保证在调用回调前已完成该 pass 所需的所有屏障与布局转换，并已按声明
 * 打开动态渲染（raster pass）。回调只需录制本 pass 的绘制命令。
 */
struct PassExecuteContext {
    /// 本 pass 录制命令的 command buffer（当前帧、已由 BeginFrame 开始录制）。
    VulkanCommandBuffer *cmd = nullptr;

    /// 当前帧（帧池缓冲/描述符分配来源）。
    VulkanRenderFrame *frame = nullptr;

    /// 渲染区域（由声明 renderArea 解析后的实际值）。
    vk::Rect2D renderArea{};

    /// 本 pass 的动态渲染信息（由图构建并已 Begin；完整定义在 VulkanRenderingInfo.h）。
    VulkanRenderingInfo *renderingInfo = nullptr;

    /// 本 pass 动态渲染实际写入的首个颜色附件视图（RenderGraph 组装 rinfo 时
    /// 使用的外部导入视图；nullptr = pass 无颜色附件）。格式经 view->get_format()
    /// 获取，无需调用方再持有 RenderTarget。
    VulkanImageView *colorAttachmentView = nullptr;

    /// 本 pass 动态渲染的深度附件视图（nullptr = 无深度附件）。
    VulkanImageView *depthAttachmentView = nullptr;
};

} // namespace GE
