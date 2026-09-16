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
     * @brief 获取或创建默认材质。
     *
     * 已存在则直接返回；否则创建一个默认（白色、无纹理）Material 并注册。
     * 供模型加载器按材质名创建命名材质使用。
     *
     * @param name 唯一名称
     * @return 材质指针
     */
    Material *GetOrCreateDefault(const std::string &name);

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

    // ========================================================================
    // `.gemat` 材质资产读写
    // ========================================================================

    /// 资产注册键前缀。与用户裸名、"scene:<内容>"、"<路径>::<材质名>" 三种
    /// 既有键空间隔离，避免路径字符串与它们相撞。
    static constexpr const char *kAssetKeyPrefix = "asset:";

    /**
     * @brief 从 `.gemat` 文件加载材质（同一路径只加载一次，之后复用实例）。
     *
     * @param resolvedPath **规范形**路径（见 AssetManager::ResolveCanonical）；
     *                     同时也是缓存键与源路径反查表的键
     * @return 材质指针，读取/解析失败返回 nullptr
     */
    Material *Load(const std::string &resolvedPath);

    /**
     * @brief 把材质写入 `.gemat` 文件（写后置源文件路径并清脏标记）。
     *
     * 目录不存在时自动创建。写成功后该材质即"文件背书"，序列化场景时写引用。
     * 资产根只读时（Android）直接失败并告警。
     *
     * @param mat          材质（会被更新源路径，故非 const）
     * @param resolvedPath 目标文件的**规范形**路径（落盘时拼在 assetRoot 之下）
     * @param assetRoot    资源根（拼写盘路径 + 贴图路径归一用）
     * @return 是否写入成功
     */
    bool Save(Material &mat, const std::string &resolvedPath, const std::string &assetRoot);

    /**
     * @brief 按源文件路径查已加载材质。
     *
     * @param resolvedPath 已解析的绝对路径
     * @return 材质指针，无则 nullptr
     */
    Material *GetBySourcePath(const std::string &resolvedPath) const;

    /**
     * @brief 把所有「有源文件且有未保存改动」的材质写回各自的源文件。
     *
     * 场景保存时调用：材质资产与场景是两份文件，只存场景而不刷材质的话，重载
     * 场景时覆写引用到的 `.gemat` 还是旧内容，用户的改动看起来又"没存住"。
     *
     * @param assetRoot 资源根（贴图路径归一用）
     * @return 实际写出的文件数
     */
    int SaveAllDirty(const std::string &assetRoot);

private:
    /// 材质缓存：名称 -> material
    std::unordered_map<std::string, std::unique_ptr<Material>> m_Materials;

    /// 源文件路径（绝对）-> 材质，`.gemat` 路径查询与另存为改路径用。
    /// 值不拥有所有权（所有权在 m_Materials），随 Unload/Clear 一并维护。
    std::unordered_map<std::string, Material *> m_ByPath;
};

} // namespace GE
