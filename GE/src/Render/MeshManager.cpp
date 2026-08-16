/**
 * @file MeshManager.cpp
 * @brief 全局网格管理器实现。
 */

#include "Render/MeshManager.h"

#include "Render/MaterialManager.h"
#include "Render/TextureManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

namespace GE {

MeshManager::MeshManager(VulkanDevice &device, MaterialManager &materialManager,
                         TextureManager &textureManager)
    : m_Device(&device), m_Materials(&materialManager), m_Textures(&textureManager) {
}

// ============================================================================
// 辅助：按 MTL 材质数据填充 Material
// ============================================================================

/**
 * @brief 根据 OBJ/MTL 捕获的材质数据填充一个 Material。
 *
 * 决定材质类型（含 PBR 扩展参数 → PBR，否则 Blinn-Phong），设置标量参数，
 * 并加载 MTL 引用的纹理（漫反射 / 法线 / 自发光）。无漫反射贴图时用
 * 漫反射颜色的纯色纹理，使 MTL 中 Kd 颜色能正确显示。
 *
 * @param mat    目标材质（填充其属性）
 * @param md     MTL 材质数据
 * @param texMgr 纹理管理器（加载纹理）
 */
static void ApplyMaterialData(Material &mat, const MaterialData &md,
                              TextureManager &texMgr) {
    // 材质类型：含 PBR 扩展参数（非零 metallic/roughness 或 MR 贴图）→ PBR
    mat.SetType(md.hasPBR ? Material::Type::PBR : Material::Type::BlinnPhong);

    // 标量参数（SetType 已按类型补齐默认值，这里覆盖为 MTL 实际值）
    if (md.hasPBR) {
        mat.SetFloat("metallic", md.metallic);
        mat.SetFloat("roughness", md.roughness);
    } else {
        mat.SetFloat("shininess", md.shininess);
        // 高光强度取高光颜色三通道均值（无高光色时趋近 0，即无高光）
        mat.SetFloat("specularStrength",
                     (md.specular.r + md.specular.g + md.specular.b) / 3.0f);
    }
    // 自发光强度取发光颜色最大通道
    mat.SetFloat("emissiveStrength",
                 glm::max(md.emissive.r, glm::max(md.emissive.g, md.emissive.b)));

    // 漫反射贴图：有 map_Kd 则加载；否则用漫反射颜色的纯色纹理（非白色时）
    if (!md.albedoMap.empty()) {
        if (Texture *t = texMgr.Load(md.albedoMap)) {
            mat.SetTexture(Material::Albedo, t);
        }
    } else {
        bool nearWhite = md.baseColor.r > 0.99f && md.baseColor.g > 0.99f
                       && md.baseColor.b > 0.99f;
        if (!nearWhite) {
            mat.SetTexture(Material::Albedo,
                           texMgr.GetSolidColor(glm::vec4(md.baseColor, md.dissolve)));
        }
    }

    // 法线贴图（map_bump / norm）
    if (!md.normalMap.empty()) {
        if (Texture *t = texMgr.Load(md.normalMap)) {
            mat.SetTexture(Material::Normal, t);
        }
    }

    // 自发光贴图（map_Ke）
    if (!md.emissiveMap.empty()) {
        if (Texture *t = texMgr.Load(md.emissiveMap)) {
            mat.SetTexture(Material::Emissive, t);
        }
    }

    // 金属-粗糙度贴图：OBJ 的 map_Pm / map_Pr 是两张独立灰度图，无法直接
    // 合并进 glTF 惯例的 MR 纹理（B=金属度, G=粗糙度），此处暂不加载，
    // 仅用标量 metallic/roughness（已在上方设置）。
    (void)md.metallicMap;
    (void)md.roughnessMap;
}

MeshManager::~MeshManager() {
    Clear();
}

Mesh *MeshManager::Get(const std::string &filepath) const {
    auto it = m_Meshes.find(filepath);
    if (it != m_Meshes.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool MeshManager::Has(const std::string &filepath) const {
    return m_Meshes.find(filepath) != m_Meshes.end();
}

Mesh *MeshManager::Load(const std::string &filepath) {
    if (filepath.empty()) {
        return nullptr;
    }

    // 已加载则直接返回缓存
    if (Mesh *existing = Get(filepath)) {
        return existing;
    }

    if (!m_Device) {
        GE_CORE_WARN("MeshManager: 无法加载网格 {}（未提供 VulkanDevice）", filepath);
        return nullptr;
    }

    std::unique_ptr<Mesh> mesh;

    // 内置几何体（builtin:cube, builtin:sphere 等）
    if (Mesh::IsBuiltinPath(filepath)) {
        mesh = Mesh::CreateBuiltin(*m_Device, Mesh::GetBuiltinType(filepath));
    } else {
        mesh = Mesh::LoadFromFile(*m_Device, filepath);
    }

    if (!mesh) {
        GE_CORE_WARN("MeshManager: 网格加载失败: {}", filepath);
        return nullptr;
    }

    // 为各子网格创建材质（放进 MaterialManager，随模型生命周期走）。
    // 材质 key 用「路径::材质名」避免跨模型同名材质冲突。
    // 有材质名（OBJ MTL）→ 按 MTL 数据构建真实材质；无材质名（内置几何体 /
    // 无 MTL 的 OBJ / CPU 直建）→ 绑一个默认（空白）材质，保证每个子网格都
    // 有材质，而非走白色 fallback。
    if (m_Materials) {
        const auto &subMeshes = mesh->GetSubMeshes();
        const auto &matData   = mesh->GetMaterialData();
        for (size_t i = 0; i < subMeshes.size(); ++i) {
            const auto &name = subMeshes[i].materialName;
            const std::string matName = name.empty() ? "default" : name;
            const std::string key = filepath + "::" + matName;

            // 按子网格材质名匹配对应的 MTL 材质数据
            const MaterialData *md = nullptr;
            if (!name.empty()) {
                for (const auto &cand : matData) {
                    if (cand.name == name) {
                        md = &cand;
                        break;
                    }
                }
            }

            // 已存在则复用（同 key 首次创建时填充），否则新建并注册
            Material *mat = m_Materials->Get(key);
            if (!mat) {
                auto newMat = std::make_unique<Material>();
                if (md && m_Textures) {
                    ApplyMaterialData(*newMat, *md, *m_Textures);
                }
                mat = m_Materials->Register(key, std::move(newMat));
            }
            mat->SetName(matName);
            mesh->SetSubMeshDefaultMaterial(static_cast<uint32_t>(i), mat);
        }
    }

    Mesh *raw = mesh.get();
    m_Meshes[filepath] = std::move(mesh);
    return raw;
}

Mesh *MeshManager::GetBuiltin(const std::string &type) {
    return Load("builtin:" + type);
}

Mesh *MeshManager::Register(const std::string &key,
                            std::unique_ptr<Mesh> mesh) {
    if (!mesh) {
        return nullptr;
    }
    Mesh *raw = mesh.get();
    m_Meshes[key] = std::move(mesh);
    return raw;
}

void MeshManager::Unload(const std::string &filepath) {
    m_Meshes.erase(filepath);
}

void MeshManager::Clear() {
    m_Meshes.clear();
}

std::vector<std::string> MeshManager::GetAllKeys() const {
    std::vector<std::string> keys;
    keys.reserve(m_Meshes.size());
    for (const auto &pair : m_Meshes) {
        keys.push_back(pair.first);
    }
    return keys;
}

} // namespace GE