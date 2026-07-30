/*
 * VulkanResourceBase — 模板成员函数显式实例化
 *
 * SetDebugName 需要 VulkanDevice 的完整定义，因此不放在头文件中内联，
 * 而是在此处实现并对所有使用到的 Handle 类型显式实例化。
 * 这样可以避免每个包含 VulkanResourceBase.h 的 TU 都需要包含 VulkanDevice.h。
 */

#include "Render/VulkanBase/VulkanResourceBase.h"
#include "Render/VulkanBase/VulkanDevice.h"

namespace GE {

template <typename Handle>
void VulkanResourceBase<Handle>::SetDebugName(const std::string &name) {
    debug_name = name;

    if (device && !debug_name.empty()) {
        device->GetDebugUtils().SetDebugName(
            device->GetHandle(),
            GetObjectType(),
            GetHandleU64(),
            debug_name.c_str());
    }
}

// ============================================================================
// 显式实例化所有使用 VulkanResourceBase 的 Handle 类型
// ============================================================================

template class VulkanResourceBase<vk::Buffer>;
template class VulkanResourceBase<vk::Image>;
template class VulkanResourceBase<vk::ShaderModule>;
template class VulkanResourceBase<vk::CommandBuffer>;
template class VulkanResourceBase<vk::Device>;
template class VulkanResourceBase<vk::ImageView>;
template class VulkanResourceBase<vk::PipelineLayout>;
template class VulkanResourceBase<vk::Pipeline>;
template class VulkanResourceBase<vk::Sampler>;

} // namespace GE
