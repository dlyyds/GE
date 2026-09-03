/**
 * @file RenderGraphTypes.h
 * @brief 渲染图（RenderGraph）基础数据类型。
 *
 * 全部为纯数据结构（无 GPU 资源、无逻辑），供 RenderGraph / ImageViewResource /
 * RenderPassDesc 头文件共用，避免相互包含。所有 vk 类型来自 vulkan.hpp。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GE {

// ============================================================================
// 资源句柄与资源分类
// ============================================================================

/// 渲染图内一个资源的唯一标识（非 0 有效）。
/// 图以索引表方式持有资源行；句柄取值 = 资源在表中的下标 + 1，0 表示无效。
using ResourceHandle = uint32_t;

/// 无效资源句柄。
constexpr ResourceHandle kInvalidResource = 0;

/// 资源种类：v1 仅图像；缓冲为阶段2（compute/transfer）预留。
enum class ResourceType {
    Image,    ///< 图像（attachment / 采样纹理 / 存储图像）
    Buffer,   ///< 缓冲（阶段2 compute/transfer 预留，v1 不可用）
};

// ============================================================================
// 资源用法（属性的单一来源）
// ============================================================================

/// 一个资源在一个 pass 里如何被访问。图据它推导依赖方向、管线阶段掩码、
/// 访问掩码与期望布局（见计划书 §7 资源用法表）。
enum class ResourceUsage {
    ColorAttachment,          ///< 颜色附件（load/store；写）
    DepthStencilAttachment,   ///< 深度/模板附件（写）
    ShaderRead,               ///< 采样/只读输入（读）
    ShaderWrite,              ///< 存储图像等写访问（阶段2 compute 预留）
    TransferDst,              ///< 传输写（阶段2 预留）
};

// ============================================================================
// 帧内虚拟资源描述（阶段2 提供实际分配；v1 仅登记描述供未来别名/优化使用）
// ============================================================================

/// 帧内虚拟图像资源的创建描述。v1 仅用于登记（Build 不真正分配 GPU 内存）。
struct RenderGraphResourceDesc {
    vk::Format             format    = vk::Format::eUndefined;    ///< 图像格式
    vk::Extent2D           extent    = {};                        ///< 尺寸
    vk::SampleCountFlagBits samples  = vk::SampleCountFlagBits::e1;
    bool                   transient = false;                     ///< 帧内临时（未来别名优化用）
};

// ============================================================================
// 附件与输入声明
// ============================================================================

/// 附件清除颜色值（loadOp == eClear 时使用）。
struct AttachmentClearValue {
    std::array<float, 4> color = {0.0f, 0.0f, 0.0f, 1.0f};  ///< 颜色清除值（默认不透明黑）
};

/// 单个附件声明：对一个资源的一次访问 + load/store 语义。
struct AttachmentDesc {
    ResourceHandle    resource = kInvalidResource;       ///< 目标资源（外部导入或虚拟）
    ResourceUsage     usage    = ResourceUsage::ColorAttachment;
    vk::AttachmentLoadOp  loadOp  = vk::AttachmentLoadOp::eLoad;
    vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eStore;
    AttachmentClearValue  clearValue{};                  ///< loadOp == eClear 时的清除值
    vk::ImageLayout finalLayout = vk::ImageLayout::eColorAttachmentOptimal;  ///< 写后布局（默认停在附件态）
};

/// 只读图像输入声明（采样）。
struct ImageInputDesc {
    ResourceHandle resource = kInvalidResource;
    ResourceUsage  usage    = ResourceUsage::ShaderRead;  ///< 恒 ShaderRead（读），保留字段备用
    vk::ImageLayout layout  = vk::ImageLayout::eShaderReadOnlyOptimal;  ///< 期望读取布局
};

// ============================================================================
// Pass 类型
// ============================================================================

/// Pass 种类。v1 仅 Raster；Compute/Transfer 为阶段2 预留。
enum class PassType {
    Raster,   ///< 动态渲染 pass（beginRendering … endRendering）
    Compute,  ///< 计算 pass（阶段2 预留）
    Transfer, ///< 传输 pass（阶段2 预留）
};

} // namespace GE
