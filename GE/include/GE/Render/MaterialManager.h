/**
 * @file MaterialManager.h
 * @brief 全局材质管理器 —— 按名称管理材质实例。
 *
 * 统一管理所有材质资源的创建与生命周期：
 * - 按名称去重存储，同名称材质共享一个实例
 * - 支持手动注册自定义材质
 * - 生命周期与 Renderer 一致（由 Renderer 持有）
 *
 * 使用方式：
 * @code
 *   auto& matMgr = Renderer::GetMaterialManager();
 *   // 注册命名材质
 *   auto mat = std::make_unique<Material>();
 *   Material* raw = matMgr.Register("MyMaterial", std::move(mat));
 *   // 获取已注册材质
 *   Material* mat2 = matMgr.Get("MyMaterial");
 * @endcode
 */

#pragma once

#include "Render/Material.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace GE {

/**
 * @brief 全局材质管理器。
 *
 * 按名称去重缓存，持有材质资源的所有权。
 */
class MaterialManager {
public:
    MaterialManager() = default;

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

    /**
     * @brief 获取所有已加载材质的名称。
     *
     * 用于编辑器 UI 下拉选择器等场景。
     */
    std::vector<std::string> GetAllNames() const;

private:
    /// 材质缓存：名称 -> material
    std::unordered_map<std::string, std::unique_ptr<Material>> m_Materials;
};

} // namespace GE
