/**
 * @file EnvironmentMap.h
 * @brief 环境映射（IBL）资源封装。
 *
 * 持有 PBR 环境光照所需的两张图：
 * - 预滤波镜面环境图（Prefiltered Env Map）：cubemap + mip 链，按粗糙度采样。
 *   同时作为漫反射辐照度使用（采样其最高 mip，近似余弦卷积）。
 * - BRDF LUT：2D 拆分（split-sum）积分表，(NoV, roughness) → (F0 系数, 菲涅尔尾项)。
 *
 * 加载完全离线：cmgen 烘焙出的 `.ktx` / `.png` 随仓库入库，引擎只负责读入并
 * 转成 Vulkan 图像，不写任何 compute 烘焙 shader。
 */

#pragma once

#include "Render/Texture.h"

#include <memory>
#include <string>

namespace GE {

class VulkanDevice;
class VulkanResourceCache;

/**
 * @brief 环境映射（IBL）资源。
 *
 * 由 LoadFromFiles 一次性加载两张图：
 *   GetPrefilter() —— 预滤波 cubemap（含 mip 链），同时绑给片元着色器的
 *     辐照度采样器（漫反射，取最高 mip）与预滤波采样器（镜面，按粗糙度取 mip）。
 *   GetBrdfLUT()   —— 2D BRDF LUT。
 */
class EnvironmentMap {
public:
    /**
     * @brief 从烘焙产物加载环境映射。
     *
     * @param device          Vulkan 设备
     * @param cache           全局资源缓存（Sampler 去重）
     * @param prefilterPath   预滤波 cubemap 的 `.ktx2` 路径（RGBA16F，与天空盒同格式）
     * @param brdfLutPath     BRDF LUT 的 `.png` 路径
     * @return std::unique_ptr<EnvironmentMap>  任一张图加载失败则返回 nullptr
     */
    static std::unique_ptr<EnvironmentMap> LoadFromFiles(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const std::string &prefilterPath,
        const std::string &brdfLutPath);

    /// 预滤波镜面环境 cubemap（含 mip 链）。
    Texture &GetPrefilter() { return *m_Prefilter; }
    const Texture &GetPrefilter() const { return *m_Prefilter; }

    /// BRDF LUT（2D，(NoV, roughness) → (F0 系数, 菲涅尔尾项)）。
    Texture &GetBrdfLUT() { return *m_BrdfLUT; }
    const Texture &GetBrdfLUT() const { return *m_BrdfLUT; }

    /// 预滤波图的 mip 级数（= 片元 MAX_REFLECTION_LOD 上限，= levelCount - 1 的 mip 索引）。
    uint32_t GetPrefilterLevels() const { return m_PrefilterLevels; }

private:
    std::unique_ptr<Texture> m_Prefilter;      ///< 预滤波镜面环境 cubemap
    std::unique_ptr<Texture> m_BrdfLUT;        ///< BRDF LUT（2D）
    uint32_t                 m_PrefilterLevels = 1; ///< 预滤波图 mip 级数
};

} // namespace GE