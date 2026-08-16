/**
 * @file Material.h
 * @brief 材质封装 —— 纹理槽位 + 标量参数 + 渲染状态。
 *
 * 材质是渲染属性的集合，包含：
 * - 纹理槽位（Albedo / Normal / Emissive 等）
 * - 标量参数（如金属度、粗糙度、自发光强度等）
 * - 渲染状态开关（alpha test、双面渲染等）
 * - 着色器类型（BlinnPhong / PBR 等）
 *
 * 与网格解耦：同一个 Material 可以被多个 Mesh / 子网格共享。
 * 材质资源由 MaterialManager 统一管理（生命周期同管理器）。
 *
 * 使用方式：
 * @code
 *   auto mat = std::make_unique<Material>();
 *   mat->SetTexture(Material::Albedo, albedoTex);
 *   mat->SetFloat("shininess", 32.0f);
 *   mat->doubleSided = false;
 * @endcode
 */

#pragma once

#include "Render/Texture.h"

#include <array>
#include <string>
#include <unordered_map>

namespace GE {

/**
 * @brief 材质封装 —— 纹理 + 参数 + 渲染状态。
 *
 * 纹理槽位预留了扩展空间，当前 Blinn-Phong 管线只使用 Albedo 槽位。
 * 后续阶段增加法线贴图、自发光等特性时，在对应槽位绑定纹理即可。
 */
class Material {
public:
    // ========================================================================
    // 纹理槽位枚举
    // ========================================================================

    /**
     * @brief 纹理槽位索引。
     *
     * 与着色器中的纹理 binding 一一对应。
     * 新增槽位时需同步更新着色器和管线布局。
     */
    enum TextureSlot : size_t {
        Albedo   = 0,   ///< 反照率（主颜色/漫反射）
        Normal   = 1,   ///< 法线贴图
        Emissive = 2,   ///< 自发光
        MetallicRoughness = 3,  ///< 金属-粗糙度贴图（glTF 惯例：B=metallic, G=roughness）
        // AmbientOcclusion   = 4,  // 环境光遮蔽
        Count
    };
    // 注：TextureSlot 是数组索引，不等同于 shader binding。当前 set 1 的
    // binding 分配为 0/1/2/3 = Albedo/Normal/MaterialUBO/Emissive，故
    // MetallicRoughness 槽位绑定在 set 1 binding 4（见 Renderer3D::EndScene）。

    /**
     * @brief 材质着色器类型。
     *
     * 决定使用哪套着色器和管线。
     */
    enum class Type {
        BlinnPhong,   ///< Blinn-Phong 光照模型（默认）
        PBR,          ///< PBR 金属-粗糙度工作流（Cook-Torrance）
    };

    // ========================================================================
    // 构造 / 析构
    // ========================================================================

    Material() {
        ApplyTypeDefaults();  // 初始化时按当前类型设置默认标量参数
    }
    ~Material() = default;

    Material(const Material &) = delete;
    Material &operator=(const Material &) = delete;
    Material(Material &&) = default;
    Material &operator=(Material &&) = default;

    // ========================================================================
    // 纹理访问
    // ========================================================================

    /**
     * @brief 设置指定槽位的纹理。
     *
     * @param slot  纹理槽位
     * @param tex   纹理指针（nullptr 表示清除该槽位）
     */
    void SetTexture(TextureSlot slot, Texture *tex) {
        m_Textures[slot] = tex;
        m_Dirty = true;
    }

    /**
     * @brief 获取指定槽位的纹理。
     *
     * @param slot  纹理槽位
     * @return 纹理指针，未设置则返回 nullptr
     */
    Texture *GetTexture(TextureSlot slot) const {
        return m_Textures[slot];
    }

    /**
     * @brief 检查指定槽位是否有纹理。
     */
    bool HasTexture(TextureSlot slot) const {
        return m_Textures[slot] != nullptr;
    }

    // ========================================================================
    // 标量参数访问
    // ========================================================================

    /**
     * @brief 设置一个浮点参数。
     *
     * @param name   参数名称
     * @param value  参数值
     */
    void SetFloat(const std::string &name, float value) {
        m_FloatParams[name] = value;
        m_Dirty = true;
    }

    /**
     * @brief 获取一个浮点参数。
     *
     * @param name       参数名称
     * @param defaultVal 未找到时的默认值
     * @return 参数值
     */
    float GetFloat(const std::string &name, float defaultVal = 0.0f) const {
        auto it = m_FloatParams.find(name);
        if (it != m_FloatParams.end()) {
            return it->second;
        }
        return defaultVal;
    }

    /**
     * @brief 检查是否存在指定名称的浮点参数。
     */
    bool HasFloat(const std::string &name) const {
        return m_FloatParams.find(name) != m_FloatParams.end();
    }

    /**
     * @brief 获取所有浮点参数（只读引用）。
     *
     * 供序列化等需要遍历全部标量参数的场景使用，避免逐一硬编码参数名。
     */
    const std::unordered_map<std::string, float> &GetFloatParams() const {
        return m_FloatParams;
    }

    // ========================================================================
    // 材质类型
    // ========================================================================

    /**
     * @brief 获取材质着色器类型。
     */
    Type GetType() const { return m_Type; }

    /**
     * @brief 设置材质着色器类型。
     *
     * 切换类型时清除旧类型的专属标量参数（不留残留），再按新类型补齐默认参数。
     * 两类共用的 emissiveStrength 保留。
     */
    void SetType(Type type) {
        if (type == m_Type) {
            return;  // 类型未变，无需处理
        }
        // 清除旧类型专属参数
        if (m_Type == Type::PBR) {
            m_FloatParams.erase("metallic");
            m_FloatParams.erase("roughness");
        } else {
            m_FloatParams.erase("shininess");
            m_FloatParams.erase("specularStrength");
        }
        m_Type = type;
        m_Dirty = true;
        ApplyTypeDefaults();
    }

    /**
     * @brief 按当前类型补齐默认标量参数（仅当参数不存在时写入，避免覆盖用户已设值）。
     *
     * - PBR：metallic（默认 0，绝缘体）、roughness（默认 0.5）
     * - Blinn-Phong：shininess（默认 32）、specularStrength（默认 0.5）
     * - 两者共用：emissiveStrength（默认 0，不发光）
     */
    void ApplyTypeDefaults() {
        if (m_Type == Type::PBR) {
            if (!HasFloat("metallic")) SetFloat("metallic", 0.0f);
            if (!HasFloat("roughness")) SetFloat("roughness", 0.5f);
        } else {
            if (!HasFloat("shininess")) SetFloat("shininess", 32.0f);
            if (!HasFloat("specularStrength")) SetFloat("specularStrength", 0.5f);
        }
        if (!HasFloat("emissiveStrength")) SetFloat("emissiveStrength", 0.0f);
        if (!HasFloat("uvTiling")) SetFloat("uvTiling", 1.0f);  // 纹理平铺（UV 缩放）密度，1 = 不平铺
    }

    // ========================================================================
    // 渲染状态（公开字段，直接修改）
    // ========================================================================

    bool alphaTest   = false;   ///< Alpha 测试（discard 低于阈值的片元）
    bool doubleSided = false;   ///< 双面渲染（禁用背面剔除）

    // ========================================================================
    // 调试 / 脏标记
    // ========================================================================

    /**
     * @brief 设置材质显示名（独立于管理器的注册 key，可自由改名不改动 manager）。
     */
    void SetName(const std::string &name) { m_Name = name; }

    /**
     * @brief 获取材质显示名。
     */
    const std::string &GetName() const { return m_Name; }

    /**
     * @brief 检查材质是否被修改（脏标记）。
     *
     * 外部系统（如材质管理器）可用于判断是否需要更新关联资源。
     */
    bool IsDirty() const { return m_Dirty; }

    /**
     * @brief 清除脏标记。
     */
    void ClearDirty() { m_Dirty = false; }

private:
    // ========================================================================
    // 成员
    // ========================================================================

    Type  m_Type = Type::BlinnPhong;   ///< 着色器类型

    /// 纹理槽位数组（裸指针，不拥有纹理资源）
    std::array<Texture *, Count> m_Textures{};

    /// 浮点参数字典
    std::unordered_map<std::string, float> m_FloatParams;

    std::string m_Name;        ///< 材质显示名（独立字段，默认 = 注册名，可自由改名）
    bool        m_Dirty = true; ///< 脏标记（构造时默认为脏，首次使用前需处理）
};

} // namespace GE
