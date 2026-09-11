#pragma once

#include "Render/RenderGraph/RenderGraph.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace GE {

/**
 * @brief 向当前帧渲染图注册场景的 3D/2D pass。
 *
 * 编辑器与运行时共用这一条链，避免两侧各自演化：
 *   - 前向：Scene3D → Scene2D
 *   - 延迟：GBuffer → Lighting → SceneColorCopy → SceneDepthCopy → Transparent →
 *           UnderwaterFX(可选) → Bloom(可选) → Tonemap → Scene2D
 *
 * 命令录制、布局转换与屏障全部由图在 Execute 时完成（计划书 §4.4：回调内不得手写屏障）。
 * 本函数只做声明，不碰任何 Scene 状态——相机矩阵与渲染批次由调用方在此之前喂给渲染器。
 *
 * @param b                当前帧图构建器（Renderer::GetFrameGraphBuilder()）
 * @param hColor           颜色输出资源句柄：
 *                         编辑器 = 离屏视口图（供 ImGui 采样显示）；运行时 = swapchain 背缓冲
 * @param hDepth           深度附件资源句柄（调用方自行 Import）
 * @param extent           渲染区域尺寸（= 颜色附件尺寸）
 * @param colorFinalLayout 末尾 Scene2D 的颜色收尾布局：
 *                         编辑器传 ShaderReadOnlyOptimal（后续 UIPass 采样视口图）；
 *                         运行时传 ColorAttachmentOptimal（后续 UIPass 继续画在同一张图上）
 * @param clearColor       未做 tonemap 时 Scene3D/Lighting 的清屏色
 */
void RecordScenePasses(RenderGraphBuilder &b,
                       ResourceHandle hColor,
                       ResourceHandle hDepth,
                       vk::Extent2D extent,
                       vk::ImageLayout colorFinalLayout,
                       const glm::vec4 &clearColor);

} // namespace GE
