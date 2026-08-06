/**
 * @file MaterialManager.h
 * @brief 全局材质管理器 —— 按名称管理材质实例。
 *
 * 统一管理所有材质资源的创建与生命周期：
 * - 按名称去重存储，同名称材质共享一个实例
 * - 支持从纹理路径快速创建单 Albedo 材质
 * - 支持手动注册自定义材质
 * - 生命周期与 Renderer 一致（由 Renderer 持有）
 *
 * 使用方式：
 * @code
 *   auto& matMgr = Renderer::GetMaterialManager();
 *   // 创建单 Albedo 材质（按纹理路径命名，自动去重）
 *   Material* mat = matMgr.GetOrCreateFromAlbedo("assets/textures/foo.png");
 *   // 获取命名材质
 *   Material* mat2 = matMgr.Get("MyCustomMaterial");
 * @endcode
 */

#pragma once

#include "Render/Material.h"
#include "Render/TextureManager.h"

#include <string>
#include <unordered_map>
#include <memory>

namespace GE {

/**
 * @brief 全局材质管理器。
 *
 * 按名称去重缓存，持有材质资源的所有权。
 * 依赖 TextureManager 来加载纹理。
 */
class MaterialManager {
public:
    /**
     * @brief 构造材质管理器。
     *
     * @param textureMgr  纹理管理器引用（用于加载纹理）
     */
    explicit MaterialManager(TextureManager &textureMgr);

    ~MaterialManager();

    MaterialManager(const MaterialManager &) = delete;
    MaterialManager &operator=(const MaterialManager &) = delete;
    MaterialManager(MaterialManager &&) = delete;
    MaterialManager &operator=(MaterialManager &&) = delete;

    // ========================================================================
    // 获取 / 创建
    // ========================================================================

    /**
     * @brief 获取已有的命名材质。
     *
     * @param name  材质名称
     * @return 材质指针，不存在返回 nullptr
     */
    Material *Get(const std::string &name) const;

    /**
     * @brief 检查是否存在指定名称的材质。
     */
    bool Has(const std::string &name) const;

    /**
     * @brief 获取或创建一个"单 Albedo 纹理"材质。
     *
     * 以纹理路径作为材质名称（内部加前缀 "albedo:"），
     * 如果已存在则直接返回，不存在则创建新材质并绑定 Albedo 槽位。
     *
     * @param albedoPath  Albedo 纹理文件路径
     * @return 材质指针，加载失败返回 nullptr
     */
    Material *GetOrCreateFromAlbedo(const std::string &albedoPath);

    /**
     * @brief 手动注册一个材质到管理器中。
     *
     * 如果 name 已存在，旧材质会被替换并销毁。
     *
     * @param name      唯一名称
     * @param material  材质指针（管理器接管所有权）
     * @return 注册后的材质指针
     */
    Material *Register(const std::string &name, std::unique_ptr<Material> material);

    /**
     * @brief 卸载指定材质。
     *
     * 注意：请确保没有任何组件还在引用该材质指针。
     */
    void Unload(const std::string &name);

    /**
     * @brief 卸载所有材质。
     */
    void Clear();

    /**
     * @brief 获取已加载材质数量。
     */
    size_t GetCount() const { return m_Materials.size(); }

private:
    TextureManager *m_TextureMgr = nullptr;  ///< 纹理管理器（不拥有）

    /// 材质缓存：名称 -> material
    std::unordered_map<std::string, std::unique_ptr<Material>> m_Materials;
};

} // namespace GE
