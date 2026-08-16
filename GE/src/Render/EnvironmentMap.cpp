/**
 * @file EnvironmentMap.cpp
 * @brief 环境映射（IBL）资源加载实现。
 */

#include "Render/EnvironmentMap.h"

#include "Core/Log.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"

#include <vulkan/vulkan.hpp>

namespace GE {

std::unique_ptr<EnvironmentMap> EnvironmentMap::LoadFromFiles(
    VulkanDevice &device,
    VulkanResourceCache &cache,
    const std::string &skyboxPath,
    const std::string &prefilterPath,
    const std::string &brdfLutPath)
{
    auto env = std::make_unique<EnvironmentMap>();

    // 天空盒 cubemap（KTX2，背景）。由环境统一持有，渲染器按方向采样。
    env->m_Skybox = Texture::LoadCubeMapFromFile(device, cache, skyboxPath);
    if (!env->m_Skybox) {
        GE_CORE_ERROR("EnvironmentMap: 天空盒加载失败: {0}", skyboxPath);
        return nullptr;
    }

    // 预滤波镜面环境 cubemap（KTX，含 mip 链）。同时作为漫反射辐照度使用。
    env->m_Prefilter = Texture::LoadCubeMapFromFile(device, cache, prefilterPath);
    if (!env->m_Prefilter) {
        GE_CORE_ERROR("EnvironmentMap: 预滤波图加载失败: {0}", prefilterPath);
        return nullptr;
    }

    // BRDF LUT（PNG，2D）。stb 按 RGBA8 解码，shader 仅采样 .rg 通道，不做 gamma 解码。
    env->m_BrdfLUT = Texture::LoadFromFile(
        device, cache, brdfLutPath,
        vk::Format::eR8G8B8A8Unorm,
        vk::Filter::eLinear, vk::Filter::eLinear,
        /*generate_mipmaps*/ false);
    if (!env->m_BrdfLUT) {
        GE_CORE_ERROR("EnvironmentMap: BRDF LUT 加载失败: {0}", brdfLutPath);
        return nullptr;
    }

    // LUT 是数据查找表，寻址用 ClampToEdge（Repeat 在 v=1 处会回绕到 rough=0 行）。
    env->m_BrdfLUT->SetAddressMode(vk::SamplerAddressMode::eClampToEdge);

    // 记录预滤波图 mip 级数，供片元着色器 MAX_REFLECTION_LOD 使用。
    env->m_PrefilterLevels = env->m_Prefilter->GetImage().get_mip_level_count();

    env->m_Skybox->SetDebugName("EnvMap_Skybox");
    env->m_Prefilter->SetDebugName("EnvMap_Prefilter");
    env->m_BrdfLUT->SetDebugName("EnvMap_BrdfLUT");

    GE_CORE_INFO("EnvironmentMap: 加载完成（天空盒 + 预滤波 {0} 级 mip）", env->m_PrefilterLevels);
    return env;
}

} // namespace GE