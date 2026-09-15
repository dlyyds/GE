/**
 * @file MaterialSerializer.cpp
 * @brief 材质 ↔ YAML 形状实现（详见头文件）。
 */

#include "Render/MaterialSerializer.h"

#include "Render/Texture.h"
#include "Render/TextureManager.h"
#include "Render/AssetManager.h"
#include "Render/AssetPathUtil.h"
#include "Render/Renderer.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <vector>

namespace GE::MaterialSerializer {
namespace {

// ============================================================================
// 向量辅助（本模块自用，不对外；与 SceneSerializer 的同名工具刻意不共享，
// 避免为一处 4 行的辅助把两个模块耦在一起）
// ============================================================================

YAML::Node MakeFlowVec3(const glm::vec3 &v) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node.push_back(v.x);
    node.push_back(v.y);
    node.push_back(v.z);
    return node;
}

YAML::Node MakeFlowVec4(const glm::vec4 &v) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node.push_back(v.x);
    node.push_back(v.y);
    node.push_back(v.z);
    node.push_back(v.w);
    return node;
}

glm::vec3 ReadVec3(const YAML::Node &node, const glm::vec3 &def) {
    if (!node || !node.IsSequence() || node.size() < 3) {
        return def;
    }
    return {node[0].as<float>(def.x), node[1].as<float>(def.y), node[2].as<float>(def.z)};
}

glm::vec4 ReadVec4(const YAML::Node &node, const glm::vec4 &def) {
    if (!node || !node.IsSequence() || node.size() < 4) {
        return def;
    }
    return {node[0].as<float>(def.x), node[1].as<float>(def.y),
            node[2].as<float>(def.z), node[3].as<float>(def.w)};
}

// ============================================================================
// 采样器参数名转换
// ============================================================================

const char *AddressModeToString(vk::SamplerAddressMode mode) {
    switch (mode) {
    case vk::SamplerAddressMode::eMirroredRepeat: return "MirroredRepeat";
    case vk::SamplerAddressMode::eClampToEdge:    return "ClampToEdge";
    case vk::SamplerAddressMode::eClampToBorder:  return "ClampToBorder";
    default:                                      return "Repeat";
    }
}

vk::SamplerAddressMode AddressModeFromString(const std::string &s) {
    if (s == "MirroredRepeat") return vk::SamplerAddressMode::eMirroredRepeat;
    if (s == "ClampToEdge")    return vk::SamplerAddressMode::eClampToEdge;
    if (s == "ClampToBorder")  return vk::SamplerAddressMode::eClampToBorder;
    return vk::SamplerAddressMode::eRepeat;
}

const char *FilterToString(vk::Filter f) {
    return (f == vk::Filter::eNearest) ? "Nearest" : "Linear";
}

vk::Filter FilterFromString(const std::string &s) {
    return (s == "Nearest") ? vk::Filter::eNearest : vk::Filter::eLinear;
}

} // namespace

std::string CanonicalAssetRef(const std::string &raw, const std::string &assetRoot) {
    if (raw.empty()) {
        return raw;
    }
    if (auto canon = AssetPathUtil::ToCanonical(raw, assetRoot)) {
        return *canon;
    }
    GE_CORE_WARN("MaterialSerializer: 资产引用无法归一为相对资源根路径，原样写入: {0}", raw);
    return raw;
}

Material::TextureSlot TextureSlotFromName(const std::string &name) {
    if (name == "Normal") return Material::Normal;
    if (name == "Emissive") return Material::Emissive;
    if (name == "MetallicRoughness") return Material::MetallicRoughness;
    return Material::Albedo;
}

const char *AlphaModeToString(Material::AlphaMode mode) {
    switch (mode) {
    case Material::AlphaMode::Mask:  return "Mask";
    case Material::AlphaMode::Blend: return "Blend";
    default:                         return "Opaque";
    }
}

Material::AlphaMode AlphaModeFromString(const std::string &s) {
    if (s == "Mask")  return Material::AlphaMode::Mask;
    if (s == "Blend") return Material::AlphaMode::Blend;
    return Material::AlphaMode::Opaque;
}

void WriteSamplerNode(YAML::Node &out, const Texture *tex) {
    if (!tex) {
        return;
    }
    out["MagFilter"]   = FilterToString(tex->GetMagFilter());
    out["MinFilter"]   = FilterToString(tex->GetMinFilter());
    out["AddressMode"] = AddressModeToString(tex->GetAddressMode());
    out["Anisotropy"]  = tex->GetAnisotropyEnabled();
}

void ApplySamplerNode(Texture *tex, const YAML::Node &node) {
    if (!tex || !node) {
        return;
    }
    if (node["MagFilter"] && node["MinFilter"]) {
        tex->SetFilter(FilterFromString(node["MagFilter"].as<std::string>("Linear")),
                       FilterFromString(node["MinFilter"].as<std::string>("Linear")));
    }
    if (node["AddressMode"]) {
        tex->SetAddressMode(AddressModeFromString(node["AddressMode"].as<std::string>("Repeat")));
    }
    if (node["Anisotropy"]) {
        tex->SetAnisotropy(node["Anisotropy"].as<bool>(false));
    }
}

void WriteMaterialNode(YAML::Node &out, const Material &mat, const std::string &assetRoot) {
    // 着色器类型，决定渲染管线
    out["Type"] = (mat.GetType() == Material::Type::PBR) ? "PBR" : "BlinnPhong";

    // 显示名。以 "scene:"/"asset:" 开头的是按内容/路径生成的注册键，不是用户命名，
    // 写出去只会把机器路径或长哈希带进文件，故跳过。
    const std::string &name = mat.GetName();
    if (!name.empty() && name.rfind("scene:", 0) != 0 && name.rfind("asset:", 0) != 0) {
        out["Name"] = name;
    }

    // 固有色：无贴图文件时的 albedo 源头（也是纯色材质唯一的颜色载体）
    out["BaseColor"] = MakeFlowVec4(mat.GetBaseColor());

    // 纹理槽位（仅写有文件路径的槽；纯色槽由 BaseColor 承载）
    for (int s = 0; s < Material::Count; ++s) {
        auto slot = static_cast<Material::TextureSlot>(s);
        const Texture *tex = mat.GetTexture(slot);
        if (tex && !tex->GetFilePath().empty()) {
            std::string texKey = std::string(kTextureSlotNames[s]) + "Texture";
            // 资产字段（内联材质覆写的四个贴图槽）——改名请同步 Scene/SceneAssetScanner.cpp
            out[texKey] = CanonicalAssetRef(tex->GetFilePath(), assetRoot);
            YAML::Node samplerNode = out[texKey + "Sampler"];
            WriteSamplerNode(samplerNode, tex);
        }
    }

    // 浮点参数。按名称排序输出：unordered_map 的遍历序不稳定，直接写会让同一材质
    // 两次保存产生不同字节，场景/材质文件无法做有意义的 diff。
    const auto &params = mat.GetFloatParams();
    if (!params.empty()) {
        std::vector<std::string> names;
        names.reserve(params.size());
        for (const auto &kv : params) {
            names.push_back(kv.first);
        }
        std::sort(names.begin(), names.end());
        YAML::Node fp = out["FloatParams"];
        for (const auto &n : names) {
            fp[n] = params.at(n);
        }
    }

    // 自发光颜色因子（两类型共用）
    out["EmissiveFactor"] = MakeFlowVec3(mat.GetEmissiveFactor());

    // 渲染状态（此前漏写这三项，编辑器里改了存不住）
    out["AlphaMode"] = AlphaModeToString(mat.alphaMode);
    if (mat.alphaMode == Material::AlphaMode::Mask) {
        out["AlphaCutoff"] = mat.alphaCutoff;
    }
    out["DoubleSided"] = mat.doubleSided;
}

void ApplyMaterialNode(Material &mat, const YAML::Node &node) {    // 类型：SetType 会清掉旧类型的专属标量参数并补齐新类型默认值，
    // 故必须先于 FloatParams 应用，文件里的实际参数才能覆盖默认值。
    mat.SetType(node["Type"] && node["Type"].as<std::string>() == "PBR"
                    ? Material::Type::PBR
                    : Material::Type::BlinnPhong);

    mat.SetBaseColor(node["BaseColor"] ? ReadVec4(node["BaseColor"], glm::vec4(1.0f))
                                      : glm::vec4(1.0f));

    auto &assetMgr = Renderer::GetAssetManager();
    auto &texMgr   = Renderer::GetTextureManager();

    for (int s = 0; s < Material::Count; ++s) {
        auto slot = static_cast<Material::TextureSlot>(s);
        const std::string texKey = std::string(kTextureSlotNames[s]) + "Texture";
        if (node[texKey]) {
            const std::string path = node[texKey].as<std::string>("");
            // 格式必须与 MeshManager::ApplyMaterialData 的约定一致：颜色贴图（albedo）
            // 以 sRGB 存储、硬件采样时解码回线性；法线/金属粗糙度是数据，保持 Unorm。
            // TextureManager 按路径缓存，两边格式不一致时先到者生效，会有一方颜色错。
            const vk::Format format = (slot == Material::Albedo)
                                          ? vk::Format::eR8G8B8A8Srgb
                                          : vk::Format::eR8G8B8A8Unorm;
            // 异步加载：返回未就绪空壳，渲染端 IsReady() 门控降级默认纹理，
            // 就绪后自动亮相，避免反序列化时主线程阻塞在纹理解码/上传
            if (Texture *tex = assetMgr.LoadTextureAsync(path, format)) {
                ApplySamplerNode(tex, node[texKey + "Sampler"]);
                mat.SetTexture(slot, tex);
            } else {
                GE_CORE_WARN("MaterialSerializer: 材质纹理异步加载失败: {0}", path);
                mat.SetTexture(slot, nullptr);
            }
        } else if (slot == Material::Albedo) {
            // 无 albedo 贴图 → 用固有色生成纯色纹理（非白时）。
            // 格式对齐 MeshManager::ApplyMaterialData：颜色以 sRGB 存储，硬件采样解码。
            const glm::vec4 bc = mat.GetBaseColor();
            mat.SetTexture(Material::Albedo,
                           bc != glm::vec4(1.0f)
                               ? texMgr.GetSolidColor(bc, vk::Format::eR8G8B8A8Srgb)
                               : nullptr);
        } else {
            mat.SetTexture(slot, nullptr);  // 节点里没有该槽 = 该材质无此贴图
        }
    }

    if (node["FloatParams"]) {
        for (const auto &it : node["FloatParams"]) {
            mat.SetFloat(it.first.as<std::string>(), it.second.as<float>());
        }
    }

    mat.SetEmissiveFactor(node["EmissiveFactor"] ? ReadVec3(node["EmissiveFactor"], glm::vec3(0.0f))
                                                 : glm::vec3(0.0f));

    mat.alphaMode = node["AlphaMode"]
                        ? AlphaModeFromString(node["AlphaMode"].as<std::string>())
                        : Material::AlphaMode::Opaque;
    mat.alphaCutoff = node["AlphaCutoff"] ? node["AlphaCutoff"].as<float>(0.5f) : 0.5f;
    mat.doubleSided = node["DoubleSided"] ? node["DoubleSided"].as<bool>(false) : false;
}

std::string NodeSignature(const YAML::Node &node) {
    YAML::Emitter emitter;
    emitter.SetSeqFormat(YAML::Flow);
    emitter.SetMapFormat(YAML::Flow);
    emitter << node;
    return emitter.c_str();
}

std::string ContentSignature(const Material &mat, const std::string &assetRoot) {
    YAML::Node node;
    WriteMaterialNode(node, mat, assetRoot);
    return NodeSignature(node);
}

} // namespace GE::MaterialSerializer
