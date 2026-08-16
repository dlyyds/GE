/**
 * @file EnvironmentMap.h
 * @brief 环境映射（IBL）资源封装。
 *
 * 持有整个环境所需的三张图：
 * - 天空盒 cubemap（Skybox）：背景，按观察方向直接采样。
 * - 预滤波镜面环境图（Prefiltered Env Map）：cubemap + mip 链，按粗糙度采样，
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
class AsyncUploadManager;

/**
 * @brief 环境映射资源。
 *
 * 由 LoadFromFiles 一次性加载三张图：
 *   GetSkybox()    —— 天空盒 cubemap（背景）。
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
     * @param skyboxPath      天空盒 cubemap 的 `.ktx2` 路径
     * @param prefilterPath   预滤波 cubemap 的 `.ktx2` 路径（RGBA16F，与天空盒同格式）
     * @param brdfLutPath     BRDF LUT 的 `.png` 路径
     * @return std::unique_ptr<EnvironmentMap>  任一张图加载失败则返回 nullptr
     */
    static std::unique_ptr<EnvironmentMap> LoadFromFiles(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const std::string &skyboxPath,
        const std::string &prefilterPath,
        const std::string &brdfLutPath);

    /**
     * @brief 异步加载环境映射（三张图后台解码 + GPU 上传）。
     *
     * 立即返回 IsReady()=false 的空壳；每张图就绪后主线程 Poll() 注入，
     * 待三张图全部就绪 IsReady() 变 true，渲染端经此门控自动亮相。
     * 任一张图解码失败则对应空壳永不就绪（IsReady 恒 false）。
     *
     * @param device          Vulkan 设备
     * @param cache           全局资源缓存（Sampler 去重）
     * @param upload          异步上传管理器
     * @param skyboxPath      天空盒 cubemap 的 `.ktx2` 路径
     * @param prefilterPath   预滤波 cubemap 的 `.ktx2` 路径
     * @param brdfLutPath     BRDF LUT 的 `.png` 路径
     * @return std::unique_ptr<EnvironmentMap>  空壳环境映射（未就绪）
     */
    static std::unique_ptr<EnvironmentMap> LoadFromFilesAsync(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        AsyncUploadManager &upload,
        const std::string &skyboxPath,
        const std::string &prefilterPath,
        const std::string &brdfLutPath);

    /// 天空盒 cubemap（背景）。
    Texture &GetSkybox() { return *m_Skybox; }
    const Texture &GetSkybox() const { return *m_Skybox; }

    /// 预滤波镜面环境 cubemap（含 mip 链）。
    Texture &GetPrefilter() { return *m_Prefilter; }
    const Texture &GetPrefilter() const { return *m_Prefilter; }

    /// BRDF LUT（2D，(NoV, roughness) → (F0 系数, 菲涅尔尾项)）。
    Texture &GetBrdfLUT() { return *m_BrdfLUT; }
    const Texture &GetBrdfLUT() const { return *m_BrdfLUT; }

    /// 预滤波图的 mip 级数（= 片元 MAX_REFLECTION_LOD 上限，= levelCount - 1 的 mip 索引）。
    /// 异步加载中就绪前返回 1（MAX_REFLECTION_LOD=0，无害过渡值）。
    uint32_t GetPrefilterLevels() const {
        return (m_Prefilter && m_Prefilter->IsReady())
                   ? m_Prefilter->GetImage().get_mip_level_count() : 1;
    }

    /**
     * @brief 环境映射是否完全就绪（三张图均可用）。
     *
     * 异步加载时三张图可能尚未完成；未就绪则渲染端应跳过天空盒与 IBL。
     */
    bool IsReady() const {
        return m_Skybox && m_Skybox->IsReady()
            && m_Prefilter && m_Prefilter->IsReady()
            && m_BrdfLUT && m_BrdfLUT->IsReady();
    }

private:
    std::unique_ptr<Texture> m_Skybox;         ///< 天空盒 cubemap
    std::unique_ptr<Texture> m_Prefilter;      ///< 预滤波镜面环境 cubemap
    std::unique_ptr<Texture> m_BrdfLUT;        ///< BRDF LUT（2D）
    uint32_t                 m_PrefilterLevels = 1; ///< 预滤波图 mip 级数
};

} // namespace GE